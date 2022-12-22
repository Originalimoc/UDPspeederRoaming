/*
 * delay_manager.h
 *
 *  Created on: Sep 15, 2017
 *      Author: root
 */

#ifndef DELAY_MANAGER_H_
#define DELAY_MANAGER_H_

#include "common.h"
#include "packet.h"
#include "log.h"

struct delay_data_t
{
	dest_t dest;
	char *data;
	int len;
	int handle();
};

struct delay_manager_t
{
	ev_timer timer;
	struct ev_loop *loop = 0;
	void (*cb)(struct ev_loop *loop, struct ev_timer *watcher, int revents) = 0;

	int capacity;
	multimap<my_time_t, delay_data_t> delay_mp; // unit us,1 us=0.001ms
	delay_manager_t();
	delay_manager_t(delay_manager_t &b)
	{
		assert(0 == 1);
	}
	void set_loop_and_cb(struct ev_loop *loop, void (*cb)(struct ev_loop *loop, struct ev_timer *watcher, int revents))
	{
		this->loop = loop;
		this->cb = cb;
		ev_init(&timer, cb);
	}
	int set_capacity(int a)
	{
		capacity = a;
		return 0;
	}
	~delay_manager_t();
	ev_timer &get_timer();
	int check();
	int add(my_time_t delay, const dest_t &dest, char *data, int len);
};

#endif /* DELAY_MANAGER_H_ */
