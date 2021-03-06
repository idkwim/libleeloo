/* 
 * Copyright (c) 2013, Quarkslab
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * - Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * - Neither the name of Quarkslab nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef LEELOO_INTERVAL_LIST_H
#define LEELOO_INTERVAL_LIST_H

#include <tbb/parallel_sort.h>

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <exception>

#include <leeloo/bench.h>
#include <leeloo/uni.h>
#include <leeloo/utility.h>

namespace leeloo {

class file_exception: public std::exception
{
public:
	file_exception():
		_errno(errno),
		_what(strerror(errno))
	{ }

public:
	const char* what() const throw() override { return _what.c_str(); }
	int get_errno() const { return _errno; }

private:
	int _errno;
	std::string _what;
};

class file_format_exception: public std::exception
{
public:
	file_format_exception(const char* msg):
		_msg(msg)
	{ }

public:
	const char* what() const throw() override { return _msg; }

private:
	const char* _msg;
};

template <class Interval, class SizeType = size_t>
class list_intervals 
{
public:
	typedef Interval interval_type;
	typedef SizeType size_type;

	typedef typename interval_type::base_type base_type;;
	typedef list_intervals<Interval, SizeType> this_type;

private:
	struct cmp_f
	{
		inline bool operator()(interval_type const& i1, interval_type const& i2) const
		{
			return i1.lower() < i2.lower();
		}
	};

public:
	typedef std::vector<interval_type> container_type;
	typedef typename container_type::const_iterator iterator;

public:
	list_intervals():
		_cache_entry_size(0)
	{ }

public:
	inline void add(base_type const a, base_type const b)
	{
		intervals().emplace_back(a, b);
	}

	inline void add(interval_type const& i)
	{
		intervals().push_back(i);
	}

	inline void remove(base_type const a, base_type const b)
	{
		removed_intervals().emplace_back(a, b);
	}

	inline void remove(interval_type const& i)
	{
		removed_intervals().push_back(i);
	}

	template <bool exclude = false>
	inline void insert(interval_type const& i)
	{
		// This will be optimised by the compiler
		if (exclude) {
			remove(i);
		}
		else {
			add(i);
		}
	}

	template <bool exclude = false>
	inline void insert(base_type const a, base_type const b)
	{
		if (exclude) {
			remove(a, b);
		}
		else {
			add(a, b);
		}
	}

	void aggregate()
	{
		if (removed_intervals().size() == 0) {
			aggregate_container(intervals());
			return;
		}

		if (intervals().size() == 0) {
			return;
		}

		aggregate_container(removed_intervals());

		tbb::parallel_sort(intervals().begin(), intervals().end(), cmp_f());

		container_type ret;
		interval_type cur_merge = intervals().front();
		typename container_type::const_iterator it_removed = removed_intervals().begin();

		typename container_type::const_iterator it = intervals().begin()+1;

		for (; it != intervals().end(); it++) {
			interval_type const& cur_int = *it;
			if (utility::overlap(cur_merge, cur_int)) {
				cur_merge = utility::hull(cur_merge, cur_int);
			}
			else {
				// split 'cur_merge' according to removed_intervals
				split_merged_interval_removed(cur_merge, ret, it_removed);
				cur_merge = cur_int;
			}
		}

		split_merged_interval_removed(cur_merge, ret, it_removed);

		// Now, merge the removed intervals, and do the final merge
		intervals() = std::move(ret);
	}

	size_type size() const
	{
		size_type ret = 0;
		for (interval_type const& i: intervals()) {
			ret += i.width();
		}
		return ret;
	}

	template <template <class T_, bool atomic_> class UPRNG, class Fset, class RandEngine>
	void random_sets(size_type size_div, Fset const& fset, RandEngine const& rand_eng) const
	{
		if (size_div <= 0) {
			size_div = 1;
		}
		const size_type size_all = size();
		UPRNG<uint32_t, false> uprng;
		uprng.init(size_all, rand_eng);

		base_type* interval_buf;
		posix_memalign((void**) &interval_buf, 16, sizeof(base_type)*size_div);

		const size_type size_all_full = (size_all/size_div)*size_div;
		for (size_type i = 0; i < size_all_full; i += size_div) {
			for (size_t j = 0; j < size_div; j++) {
				interval_buf[j] = at_cached(uprng());
			}
			fset(interval_buf, size_div);
		}

		const size_type rem = size_all-size_all_full;
		if (rem > 0) {
			for (size_type i = size_all_full; i < size_all; i++) {
				interval_buf[i-size_all_full] = at_cached(uprng());
			}
			fset(interval_buf, rem);
		}

		free(interval_buf);
	}

	template <class Fset, class RandEngine>
	inline void random_sets(size_type size_div, Fset const& fset, RandEngine const& rand_eng) const
	{
		random_sets<uni>(size_div, fset, rand_eng);
	}

	inline void reserve(size_type n) { intervals().reserve(n); }
	inline void clear() { intervals().clear(); removed_intervals().clear(); }

	inline container_type const& intervals() const { return _intervals; }

	inline base_type at(size_type const r) const
	{
		assert(r < size());
		// Dummy algorithm, should make a better one with cached indexes
		return get_rth_value(r, 0, intervals().size());
	}

	base_type at_cached(size_type const r) const
	{
		assert(r < size() && _cache_entry_size > 0);
		ssize_t cur;
		const size_t interval_idx = get_cached_interval_idx(r, cur);
		if (cur == 0) {
			return intervals()[interval_idx].lower();
		}
		return get_rth_value(cur, interval_idx, intervals().size());
	}

	// cache_entry_size defines the number of intervals that represent a cache entry
	void create_index_cache(size_t const cache_entry_size)
	{
		assert(cache_entry_size > 0);
		_cache_entry_size = cache_entry_size;
		const size_t intervals_count = intervals().size();
		size_type cur_size = 0;
		_index_cache.clear();
		_index_cache.reserve((intervals_count+cache_entry_size-1)/cache_entry_size);
		for (size_t i = 0; i < intervals_count; i += cache_entry_size) {
			for (size_t j = i; j < i+cache_entry_size; j++) {
				cur_size += intervals()[j].width();
			}
			_index_cache.push_back(cur_size);
		}
	}

	bool operator==(this_type const& o) const
	{
		if (_intervals.size() != o._intervals.size()) {
			return false;
		}

		for (size_t i = 0; i < _intervals.size(); i++) {
			if ((_intervals[i].lower() != o._intervals[i].lower()) ||
				(_intervals[i].upper() != o._intervals[i].upper())) {
				return false;
			}
		}

		return true;
	}

	inline bool operator!=(this_type const& o) const
	{
		return !operator==(o);
	}

	bool contains(base_type const v) const
	{
		// Suppose that intervals have been aggregated! (and are thus sorted)
		size_type a = 0;
		size_type b = intervals().size(); 

		while ((b-a) > 4) {
			const size_type mid = (b+a)/2;
			interval_type const& it = intervals()[mid];
			if (it.contains(v)) {
				return true;
			}
			if (v < it.lower()) {
				b = mid;
			}
			else {
				a = mid;
			}
		}

		for (size_type i = a; i < b; i++) {
			if (intervals()[i].contains(v)) {
				return true;
			}
		}

		return false;
	}

public:
	void dump_to_file(const char* file)
	{
		int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
		if (fd == -1) {
			throw file_exception();
		}

		size_t size_write = intervals().size()*sizeof(interval_type);
		ssize_t w = write(fd, &intervals()[0], size_write);
		if ((w < 0) ||
		    (((size_t)w) != size_write)) {
			throw file_exception();
		}
		close(fd);
	}

	void read_from_file(const char* file)
	{
		int fd = open(file, O_RDONLY);
		if (fd == -1) {
			throw file_exception();
		}

		off_t size_file = lseek(fd, 0, SEEK_END);
		if (size_file == -1) {
			throw file_exception();
		}
		if (lseek(fd, 0, SEEK_SET) == -1) {
			throw file_exception();
		}

		if (size_file % sizeof(interval_type) != 0) {
			throw file_format_exception("invalid size");
		}

		size_t n = size_file/sizeof(interval_type);
		clear();
		intervals().resize(n);
		ssize_t r = read(fd, &intervals()[0], size_file);
		if (r < 0) {
			throw file_exception();
		}
		if (((size_t)r) != size_file) {
			throw file_exception();
		}
		close(fd);

		for (interval_type const& i: intervals()) {
			if (i.lower() >= i.upper()) {
				throw file_format_exception("invalid interval");
			}
		}
	}

private:
	static void aggregate_container(container_type& ints)
	{
		if (ints.size() <= 1) {
			return;
		}
		
		tbb::parallel_sort(ints.begin(), ints.end(), cmp_f());

		container_type ret;
		interval_type cur_merge = ints.front();

		typename container_type::const_iterator it = ints.begin()+1;

		// TODO: there is plenty of room for memory optimisations,
		// vectorisation and //isation ;)
		for (; it != ints.end(); it++) {
			interval_type const& cur_int = *it;
			if (utility::overlap(cur_merge, cur_int)) {
				cur_merge = utility::hull(cur_merge, cur_int);
			}
			else {
				ret.push_back(cur_merge);
				cur_merge = cur_int;
			}
		}

		ret.push_back(cur_merge);

		ints = std::move(ret);
	}

	void split_merged_interval_removed(interval_type const& cur_merge_, container_type& ret, typename container_type::const_iterator& it_removed)
	{
		bool notend;
		while (((notend = it_removed != removed_intervals().end())) &&
				(cur_merge_.lower() > it_removed->upper())) {
			it_removed++;
		}

		interval_type cur_merge = cur_merge_;
		if (notend) {
			typename container_type::const_iterator it_removed_cur = it_removed;
			do {
				const base_type rem_lower = it_removed_cur->lower();
				const base_type rem_upper = it_removed_cur->upper();
				if ((cur_merge.lower() >= rem_lower) &&
					(cur_merge.upper() <= rem_upper)) {
					cur_merge = interval_type::empty();
				}
				else
				if ((cur_merge.lower() >= rem_lower) &&
					(cur_merge.upper() >= rem_upper) &&
					(cur_merge.lower() <= rem_upper)) {
					cur_merge.set_lower(rem_upper);
				}
				else
				if ((cur_merge.lower() <= rem_lower) &&
					(cur_merge.upper() <= rem_upper) &&
					(cur_merge.upper() >= rem_lower)) {
					cur_merge.set_upper(rem_lower);
				}
				else
				if ((cur_merge.lower() <= rem_lower) &&
					(cur_merge.upper() >= rem_upper)) {
					ret.emplace_back(cur_merge.lower(), rem_lower);
					cur_merge.set_lower(rem_upper);
				}

				it_removed_cur++;
			}
			while ((it_removed_cur != removed_intervals().end()) && (it_removed_cur->lower() < cur_merge.upper()));
		}

		if (cur_merge.width() > 0) {
			ret.push_back(cur_merge);
		}
	}

	size_type get_rth_value(size_type const r, size_t const interval_start, size_t const interval_end) const
	{
		// [interval_start,interval_end[
		ssize_t cur = r;
		for (size_t i = interval_start; i < interval_end; i++) {
			const interval_type it = intervals()[i];
			cur -= it.width();
			if (cur < 0) {
				// This is it!
				return it.upper() + cur;
			}
		}
		return -1;
	}
	size_t get_cached_interval_idx(size_type const r, ssize_t& rem) const
	{
		// Dichotomy!
		size_t a = 0;
		size_t b = _index_cache.size();

		// Tradeoff
		while ((b-a) > 4) {
			const size_t mid = (b+a)/2;
			const size_type vmid = _index_cache[mid];
			if (vmid == r) {
				// You get lucky
				rem = 0;
				return (mid+1)*_cache_entry_size;
			}
			if (vmid < r) {
				a = mid;
			}
			else {
				b = mid;
			}
		}

		// Finish this!
		size_type v;
		while ((v = _index_cache[a]) < r) {
			a++;
		}

		if (a > 0) {
			rem = (ssize_t)r-(ssize_t)_index_cache[a-1];
		}
		else {
			rem = r;
		}

		return a*_cache_entry_size;
	}

public:
	iterator begin() const { return intervals().begin(); }
	iterator end() const { return intervals().end(); }

private:
	inline container_type& intervals() { return _intervals; }
	inline container_type& removed_intervals() { return _excluded_intervals; }

private:
	container_type _intervals;
	container_type _excluded_intervals;
	std::vector<size_type> _index_cache;
	size_t _cache_entry_size;
	bool _cache_valid;
};

}

#endif
