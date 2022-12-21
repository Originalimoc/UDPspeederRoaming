#include "tunnel.h"

void data_from_local_or_fec_timeout(conn_info_t &conn_info, int is_time_out)
{
	fd64_t &remote_fd64 = conn_info.remote_fd64;
	int &local_listen_fd = conn_info.local_listen_fd;

	char data[buf_len];
	int data_len;
	address_t addr;
	u32_t conv;
	int out_n;
	char **out_arr;
	int *out_len;
	my_time_t *out_delay;
	dest_t dest;
	dest.type = type_fd_addr;
	dest.inner.fd_addr.fd = conn_info.remote_fd;
	//or this
	//dest.inner.fd_addr.fd = fd_manager.to_fd(remote_fd64);
	//assert(conn_info.remote_fd == fd_manager.to_fd(remote_fd64)) passed
	dest.inner.fd_addr.addr = remote_addr;
	dest.cook = 1;

	if (is_time_out)
	{
		// fd64_t fd64=events[idx].data.u64;
		mylog(log_trace, "events[idx].data.u64 == conn_info.fec_encode_manager.get_timer_fd64()\n");

		from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
	}
	else // events[idx].data.u64 == (u64_t)local_listen_fd
	{
		mylog(log_trace, "events[idx].data.u64 == (u64_t)local_listen_fd\n");
		address_t::storage_t udp_new_addr_in = {0};
		socklen_t udp_new_addr_len = sizeof(address_t::storage_t);
		if ((data_len = recvfrom(local_listen_fd, data, max_data_len + 1, 0,
								 (struct sockaddr *)&udp_new_addr_in, &udp_new_addr_len)) == -1)
		{
			mylog(log_debug, "recv_from error,this shouldnt happen,err=%s,but we can try to continue\n", get_sock_error());
			return;
		};

		if (data_len == max_data_len + 1)
		{
			mylog(log_warn, "huge packet from upper level, data_len > %d, packet truncated, dropped\n", max_data_len);
			return;
		}

		if (!disable_mtu_warn && data_len >= mtu_warn)
		{
			mylog(log_warn, "huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
		}

		addr.from_sockaddr((struct sockaddr *)&udp_new_addr_in, udp_new_addr_len);

		mylog(log_trace, "Received packet from %s, len: %d\n", addr.get_str(), data_len);

		// u64_t u64=ip_port.to_u64();

		if (!conn_info.conv_manager.c.is_data_used(addr))
		{
			if (conn_info.conv_manager.c.get_size() >= max_conv_num)
			{
				mylog(log_warn, "ignored new udp connect bc max_conv_num exceed\n");
				return;
			}
			conv = conn_info.conv_manager.c.get_new_conv();
			conn_info.conv_manager.c.insert_conv(conv, addr);
			mylog(log_info, "new packet from %s,conv_id=%x\n", addr.get_str(), conv);
		}
		else
		{
			conv = conn_info.conv_manager.c.find_conv_by_data(addr);
			mylog(log_trace, "conv=%d\n", conv);
		}
		conn_info.conv_manager.c.update_active_time(conv);
		char *new_data;
		int new_len;
		put_conv(conv, data, data_len, new_data, new_len);

		mylog(log_trace, "data_len=%d new_len=%d\n", data_len, new_len);
		from_normal_to_fec(conn_info, new_data, new_len, out_n, out_arr, out_len, out_delay);
	}
	mylog(log_trace, "out_n=%d\n", out_n);
	for (int i = 0; i < out_n; i++)
	{
		delay_send(out_delay[i], dest, out_arr[i], out_len[i]);
	}
}
static void local_listen_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);

	data_from_local_or_fec_timeout(conn_info, 0);
}

static void remote_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);

	char data[buf_len];
	if (!fd_manager.exist(watcher->u64)) // fd64 has been closed
	{
		mylog(log_trace, "!fd_manager.exist(events[idx].data.u64)");
		return;
	}
	fd64_t &remote_fd64 = conn_info.remote_fd64;
	int &remote_fd = conn_info.remote_fd;

	assert(watcher->u64 == remote_fd64);

	int fd = fd_manager.to_fd(remote_fd64);

	int data_len = recv(fd, data, max_data_len + 1, 0);

	if (data_len == max_data_len + 1)
	{
		mylog(log_warn, "huge packet, data_len > %d, packet truncated, dropped\n", max_data_len);
		return;
	}

	mylog(log_trace, "received data from udp fd %d, len=%d\n", remote_fd, data_len);
	if (data_len < 0)
	{
		if (get_sock_errno() == ECONNREFUSED)
		{
			mylog(log_debug, "recv failed %d ,udp_fd%d,errno:%s\n", data_len, remote_fd, get_sock_error());
		}

		mylog(log_warn, "recv failed %d ,udp_fd%d,errno:%s\n", data_len, remote_fd, get_sock_error());
		return;
	}
	if (!disable_mtu_warn && data_len > mtu_warn)
	{
		mylog(log_warn, "huge packet,data len=%d (>%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
	}

	if (de_cook(data, data_len) != 0)
	{
		mylog(log_debug, "de_cook error");
		return;
	}

	int out_n;
	char **out_arr;
	int *out_len;
	my_time_t *out_delay;
	from_fec_to_normal(conn_info, data, data_len, out_n, out_arr, out_len, out_delay);

	mylog(log_trace, "out_n=%d\n", out_n);

	for (int i = 0; i < out_n; i++)
	{
		u32_t conv;
		char *new_data;
		int new_len;
		if (get_conv(conv, out_arr[i], out_len[i], new_data, new_len) != 0)
		{
			mylog(log_debug, "get_conv(conv,out_arr[i],out_len[i],new_data,new_len)!=0");
			continue;
		}
		if (!conn_info.conv_manager.c.is_conv_used(conv))
		{
			mylog(log_trace, "!conn_info.conv_manager.is_conv_used(conv)");
			continue;
		}

		conn_info.conv_manager.c.update_active_time(conv);

		address_t addr = conn_info.conv_manager.c.find_data_by_conv(conv);
		dest_t dest;
		dest.type = type_fd_addr;
		dest.inner.fd_addr.fd = conn_info.local_listen_fd;
		dest.inner.fd_addr.addr = addr;

		delay_send(out_delay[i], dest, new_data, new_len);
	}
}

static void fifo_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	assert(!(revents & EV_ERROR));
	int fifo_fd = watcher->fd;

	char buf[buf_len];
	int len = read(fifo_fd, buf, sizeof(buf));
	if (len < 0)
	{
		mylog(log_warn, "fifo read failed len=%d,errno=%s\n", len, get_sock_error());
		return;
	}
	buf[len] = 0;
	handle_command(buf);
}

static void delay_manager_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	assert(!(revents & EV_ERROR));
}

static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);

	data_from_local_or_fec_timeout(conn_info, 1);
}

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	uint64_t value;

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);

	conn_info.conv_manager.c.clear_inactive();
	mylog(log_trace, "events[idx].data.u64==(u64_t)conn_info.timer.get_timer_fd()\n");

	conn_info.stat.report_as_client();

	//MOD Note: this code block is a duplicate of above, so makes sense
	if (debug_force_flush_fec)
	{
		int out_n;
		char **out_arr;
		int *out_len;
		my_time_t *out_delay;
		dest_t dest;
		dest.type = type_fd_addr;
		dest.inner.fd_addr.fd = conn_info.remote_fd;
		dest.inner.fd_addr.addr = remote_addr;
		dest.cook = 1;
		from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
		for (int i = 0; i < out_n; i++)
		{
			delay_send(out_delay[i], dest, out_arr[i], out_len[i]);
		}
	}
}

static void prepare_cb(struct ev_loop *loop, struct ev_prepare *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	delay_manager.check();
}

int tunnel_client_event_loop()
{
	int i, j, k;
	int ret;
	int yes = 1;

	conn_info_t *conn_info_p = new conn_info_t;
	conn_info_t &conn_info = *conn_info_p; // huge size of conn_info,do not allocate on stack

	int &local_listen_fd = conn_info.local_listen_fd;
	new_listen_socket2(local_listen_fd, local_addr);

	struct ev_loop *loop = ev_default_loop(0);
	assert(loop != NULL);

	conn_info.loop = loop;

	struct ev_io local_listen_watcher;
	local_listen_watcher.data = &conn_info;

	ev_io_init(&local_listen_watcher, local_listen_cb, local_listen_fd, EV_READ);
	ev_io_start(loop, &local_listen_watcher);

	int &remote_fd = conn_info.remote_fd;
	fd64_t &remote_fd64 = conn_info.remote_fd64;

	assert(new_connected_socket2(remote_fd, remote_addr, out_addr, 1) == 0);
	remote_fd64 = fd_manager.create(remote_fd);

	mylog(log_debug, "remote_fd64=%llu\n", remote_fd64);

	struct ev_io remote_watcher;
	remote_watcher.data = &conn_info;
	remote_watcher.u64 = remote_fd64;

	ev_io_init(&remote_watcher, remote_cb, remote_fd, EV_READ);
	ev_io_start(loop, &remote_watcher);

	delay_manager.set_loop_and_cb(loop, delay_manager_cb);

	conn_info.fec_encode_manager.set_data(&conn_info);
	conn_info.fec_encode_manager.set_loop_and_cb(loop, fec_encode_cb);

	conn_info.timer.data = &conn_info;
	ev_init(&conn_info.timer, conn_timer_cb);
	ev_timer_set(&conn_info.timer, 0, timer_interval / 1000.0);
	ev_timer_start(loop, &conn_info.timer);

	struct ev_io fifo_watcher;

	int fifo_fd = -1;

	if (fifo_file[0] != 0)
	{
		fifo_fd = create_fifo(fifo_file);

		mylog(log_info, "fifo_file=%s\n", fifo_file);

		ev_io_init(&fifo_watcher, fifo_cb, fifo_fd, EV_READ);
		ev_io_start(loop, &fifo_watcher);
	}

	ev_prepare prepare_watcher;
	ev_init(&prepare_watcher, prepare_cb);
	ev_prepare_start(loop, &prepare_watcher);

	mylog(log_info, "now listening at %s\n", local_addr.get_str());

	ev_run(loop, 0);

	mylog(log_warn, "ev_run returned\n");
	myexit(0);

	return 0;
}
