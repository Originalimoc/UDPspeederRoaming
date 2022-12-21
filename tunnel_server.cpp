/*
 * tunnel.cpp
 *
 *  Created on: Oct 26, 2017
 *      Author: root
 */

#include "tunnel.h"

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents);
static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents);
static void remote_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);

enum tmp_mode_t
{
	is_from_remote = 0,
	is_fec_timeout,
	is_conn_timer
};

void data_from_remote_or_fec_timeout_or_conn_timer(conn_info_t &conn_info, fd64_t fd64, tmp_mode_t mode)
{
	int ret;

	char data[buf_len];
	int data_len;
	u32_t conv;

	address_t &addr = conn_info.addr;
	assert(conn_manager.exist(addr));

	int &local_listen_fd = conn_info.local_listen_fd;

	int out_n = -2;
	char **out_arr;
	int *out_len;
	my_time_t *out_delay;

	dest_t dest;
	dest.inner.fd_addr.fd = local_listen_fd;
	dest.inner.fd_addr.addr = addr;
	dest.type = type_fd_addr;
	dest.cook = 1;

	if (mode == is_fec_timeout)
	{
		assert(fd64 == 0);

		from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
	}
	else if (mode == is_conn_timer)
	{
		assert(fd64 == 0);

		conn_info.conv_manager.s.clear_inactive();
		if (debug_force_flush_fec)
		{
			from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
		}

		conn_info.stat.report_as_server(addr);
		return;
	}
	else if (mode == is_from_remote)
	{
		if (!fd_manager.exist(fd64)) // fd64 has been closed
		{
			mylog(log_warn, "!fd_manager.exist(fd64)\n");
			return;
		}

		assert(conn_info.conv_manager.s.is_data_used(fd64));

		conv = conn_info.conv_manager.s.find_conv_by_data(fd64);
		conn_info.conv_manager.s.update_active_time(conv);
		conn_info.update_active_time();

		int fd = fd_manager.to_fd(fd64);
		data_len = recv(fd, data, max_data_len + 1, 0);

		if (data_len == max_data_len + 1)
		{
			mylog(log_warn, "huge packet from upper level, data_len > %d, packet truncated, dropped\n", max_data_len);
			return;
		}

		mylog(log_trace, "received a packet from udp_fd,len:%d,conv=%d\n", data_len, conv);

		if (data_len < 0)
		{
			mylog(log_debug, "udp fd,recv_len<0 continue,%s\n", get_sock_error());

			return;
		}

		if (!disable_mtu_warn && data_len >= mtu_warn)
		{
			mylog(log_warn, "huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
		}

		char *new_data;
		int new_len;
		put_conv(conv, data, data_len, new_data, new_len);

		from_normal_to_fec(conn_info, new_data, new_len, out_n, out_arr, out_len, out_delay);
	}
	else
	{
		assert(0 == 1);
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

	int local_listen_fd = watcher->fd;
	int ret;

	mylog(log_trace, "events[idx].data.u64 == (u64_t)local_listen_fd\n");
	char data[buf_len];
	int data_len;
	address_t::storage_t udp_new_addr_in = {0};
	socklen_t udp_new_addr_len = sizeof(address_t::storage_t);
	if ((data_len = recvfrom(local_listen_fd, data, max_data_len + 1, 0,
							 (struct sockaddr *)&udp_new_addr_in, &udp_new_addr_len)) == -1)
	{
		mylog(log_error, "recv_from error,this shouldnt happen,err=%s,but we can try to continue\n", get_sock_error());
		return;
	};

	if (data_len == max_data_len + 1)
	{
		mylog(log_warn, "huge packet, data_len > %d, packet truncated, dropped\n", max_data_len);
		return;
	}

	address_t addr;
	addr.from_sockaddr((struct sockaddr *)&udp_new_addr_in, udp_new_addr_len);

	mylog(log_trace, "Received packet from %s,len: %d\n", addr.get_str(), data_len);

	if (!disable_mtu_warn && data_len >= mtu_warn) ///////////////////////delete this for type 0 in furture
	{
		mylog(log_warn, "huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
	}

	if (de_cook(data, data_len) != 0)
	{
		mylog(log_debug, "de_cook error");
		return;
	}

	if (!conn_manager.exist(addr))
	{
		if (conn_manager.mp.size() >= max_conn_num)
		{
			mylog(log_warn, "new connection %s ignored bc max_conn_num exceed\n", addr.get_str());
			return;
		}

		conn_info_t &conn_info = conn_manager.find_insert(addr);
		conn_info.addr = addr;
		conn_info.loop = ev_default_loop(0);
		conn_info.local_listen_fd = local_listen_fd;

		conn_info.timer.data = &conn_info;
		ev_init(&conn_info.timer, conn_timer_cb);
		ev_timer_set(&conn_info.timer, 0, timer_interval / 1000.0);
		ev_timer_start(loop, &conn_info.timer);

		conn_info.fec_encode_manager.set_data(&conn_info);
		conn_info.fec_encode_manager.set_loop_and_cb(loop, fec_encode_cb);

		mylog(log_info, "new connection from %s\n", addr.get_str());
	}
	conn_info_t &conn_info = conn_manager.find_insert(addr);

	conn_info.update_active_time();
	int out_n;
	char **out_arr;
	int *out_len;
	my_time_t *out_delay;
	from_fec_to_normal(conn_info, data, data_len, out_n, out_arr, out_len, out_delay);

	mylog(log_trace, "out_n= %d\n", out_n);
	for (int i = 0; i < out_n; i++)
	{
		u32_t conv;
		char *new_data;
		int new_len;
		if (get_conv(conv, out_arr[i], out_len[i], new_data, new_len) != 0)
		{
			mylog(log_debug, "get_conv failed");
			continue;
		}

		if (!conn_info.conv_manager.s.is_conv_used(conv))
		{
			if (conn_info.conv_manager.s.get_size() >= max_conv_num)
			{
				mylog(log_warn, "ignored new udp connect bc max_conv_num exceed\n");
				continue;
			}

			int new_udp_fd;

			if (new_connected_socket2(new_udp_fd, remote_addr, out_addr, 0) != 0)
			{
				mylog(log_warn, "[%s]new_connected_socket failed\n", addr.get_str());
				continue;
			}

			fd64_t fd64 = fd_manager.create(new_udp_fd);

			conn_info.conv_manager.s.insert_conv(conv, fd64);
			fd_manager.get_info(fd64).addr = addr;

			ev_io &io_watcher = fd_manager.get_info(fd64).io_watcher;
			io_watcher.u64 = fd64;
			io_watcher.data = &conn_info;

			ev_init(&io_watcher, remote_cb);
			ev_io_set(&io_watcher, new_udp_fd, EV_READ);
			ev_io_start(conn_info.loop, &io_watcher);

			mylog(log_info, "[%s]new conv %x,fd %d created,fd64=%llu\n", addr.get_str(), conv, new_udp_fd, fd64);
		}
		conn_info.conv_manager.s.update_active_time(conv);
		fd64_t fd64 = conn_info.conv_manager.s.find_data_by_conv(conv);
		dest_t dest;
		dest.type = type_fd64;
		dest.inner.fd64 = fd64;
		delay_send(out_delay[i], dest, new_data, new_len);
	}
}

static void remote_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);
	fd64_t fd64 = watcher->u64;

	data_from_remote_or_fec_timeout_or_conn_timer(conn_info, fd64, is_from_remote);
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

	data_from_remote_or_fec_timeout_or_conn_timer(conn_info, 0, is_fec_timeout);
}

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_info_t &conn_info = *((conn_info_t *)watcher->data);

	data_from_remote_or_fec_timeout_or_conn_timer(conn_info, 0, is_conn_timer);
}

static void prepare_cb(struct ev_loop *loop, struct ev_prepare *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	delay_manager.check();
}

static void global_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	assert(!(revents & EV_ERROR));

	conn_manager.clear_inactive();
	mylog(log_trace, "events[idx].data.u64==(u64_t)timer.get_timer_fd()\n");
}

int tunnel_server_event_loop()
{

	int i, j, k;
	int ret;
	int yes = 1;

	int local_listen_fd;
	new_listen_socket2(local_listen_fd, local_addr);

	struct ev_loop *loop = ev_default_loop(0);
	assert(loop != NULL);

	struct ev_io local_listen_watcher;
	ev_io_init(&local_listen_watcher, local_listen_cb, local_listen_fd, EV_READ);
	ev_io_start(loop, &local_listen_watcher);

	delay_manager.set_loop_and_cb(loop, delay_manager_cb);

	mylog(log_info, "now listening at %s\n", local_addr.get_str());

	ev_timer global_timer;
	ev_init(&global_timer, global_timer_cb);
	ev_timer_set(&global_timer, 0, timer_interval / 1000.0);
	ev_timer_start(loop, &global_timer);

	struct ev_io fifo_watcher;

	int fifo_fd = -1;

	if (fifo_file[0] != 0)
	{
		fifo_fd = create_fifo(fifo_file);
		ev_io_init(&fifo_watcher, fifo_cb, fifo_fd, EV_READ);
		ev_io_start(loop, &fifo_watcher);
		mylog(log_info, "fifo_file=%s\n", fifo_file);
	}

	ev_prepare prepare_watcher;
	ev_init(&prepare_watcher, prepare_cb);
	ev_prepare_start(loop, &prepare_watcher);
	ev_run(loop, 0);
	mylog(log_warn, "ev_run returned\n");
	myexit(0);

	return 0;
}
