//
// Copyright (C) 2013 University of Kaiserslautern
// Microelectronic Systems Design Research Group
//
// Christian Brugger (brugger@eit.uni-kl.de)
// 20. August 2013
//

#include "iodev.hpp"

#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <unistd.h>

#include <thread>
#include <chrono>
#include <iostream>
#include <cmath>
#include <fstream>

const unsigned PACKET_SIZE = 500;
const useconds_t BUSY_WAIT_NANO_SLEEP_TIME = 0; // nano seconds
const useconds_t BUSY_WAIT_MICRO_SLEEP_TIME = 50; // micro seconds

// address of AXI Stream FIFO
const unsigned AXI_FIFO_BASE_ADDR   = 0x4AA00000;
const unsigned AXI_FIFO_SIZE        = 0x00001000;
// address of Random Number Generator
const unsigned AXI_RNG_BASE_ADDR    = 0x43C00000;
const unsigned AXI_RNG_SIZE         = 0x00002000;
// address of Heston Kernel
const unsigned AXI_HESTON_BASE_ADDR = 0x43C10000;
const unsigned AXI_HESTON_SIZE      = 0x00000080;


// sleep for nano_sec micro seconds with busy loop
void nano_sleep(long nano_sec) {
	auto start = std::chrono::steady_clock::now();
	while (true) {
		long nano = std::chrono::duration<long, std::nano>(
			std::chrono::steady_clock::now() - start).count();
		if (nano_sec <= nano)
			break;
	}
}


void print_duration(std::chrono::steady_clock::time_point start, 
		std::chrono::steady_clock::time_point end, unsigned cnt) {
	double duration = std::chrono::duration<double>(
		end - start).count();
	std::cout << "Calculated " << cnt << " values in " << duration
		<< " seconds (" << cnt / duration << " values / sec)"
		<< std::endl;
}


void call_write_thread(const unsigned cnt, const bool do_busy_wait) {
	IODev axi_ctrl(AXI_FIFO_BASE_ADDR, AXI_FIFO_SIZE);
	// Transmit Data FIFO Vacancy (TDFV)
	volatile unsigned &tdfv = *((unsigned*)axi_ctrl.get_dev_ptr(0x0C));
	// Transmit Data FIFO Data (TDFD)
	volatile unsigned &tdfd = *((unsigned*)axi_ctrl.get_dev_ptr(0x10));
	// Transmit Length FIFO (TLF)
	volatile unsigned &tlf = *((unsigned*)axi_ctrl.get_dev_ptr(0x14));

	unsigned cnt_todo = cnt;
	unsigned free = 0;
	for (unsigned i = 0; i < cnt; ++i) {
		while (free == 0) {
			free = tdfv;
			if (do_busy_wait)
				nano_sleep(BUSY_WAIT_NANO_SLEEP_TIME);
			else
				usleep(BUSY_WAIT_MICRO_SLEEP_TIME);
		}
		tdfd = i;
		--free;
		if ((i+1) % PACKET_SIZE == 0) {
			tlf = PACKET_SIZE * 4;
			cnt_todo -= PACKET_SIZE;
		}
	}
	if (cnt_todo > 0) {
		tlf = cnt_todo * 4;
	}
}


//TODO(brugger): use void* for out and cnt in bytes instead of sizeof(unsigned)
void call_read_thread(const unsigned cnt, unsigned *out, bool do_busy_wait) {
	IODev axi_ctrl(AXI_FIFO_BASE_ADDR, AXI_FIFO_SIZE);
	// Receive Data FIFO Occupancy (RDFO)
	volatile unsigned &rdfo = *((unsigned*)axi_ctrl.get_dev_ptr(0x1C));
	// Receive Data FIFO Data (RDFD)
	volatile unsigned &rdfd = *((unsigned*)axi_ctrl.get_dev_ptr(0x20));

	unsigned avail = 0;
	for (unsigned i = 0; i < cnt; ++i) {
		while (avail == 0) {
			avail = rdfo & 0x7fffffff;
			if (do_busy_wait)
				nano_sleep(BUSY_WAIT_NANO_SLEEP_TIME);
			else
				usleep(BUSY_WAIT_MICRO_SLEEP_TIME);
		}
		out[i] = rdfd;
		--avail;
	}
}


template <typename T>
class read_iterator {
public:
	read_iterator(const unsigned cnt, bool do_busy_wait=false) 
		: cnt(cnt), 
		  do_busy_wait(do_busy_wait), 
		  axi_ctrl(AXI_FIFO_BASE_ADDR, AXI_FIFO_SIZE)
	{
		// Receive Data FIFO Occupancy (RDFO)
		rdfo = (unsigned*)axi_ctrl.get_dev_ptr(0x1C);
		// Receive Data FIFO Data (RDFD)
		rdfd = (T*)axi_ctrl.get_dev_ptr(0x20);
		if (sizeof(T) != 4) {
			std::cerr << "ERROR: only sizeof(T) = 4 supported" 
					<< std::endl;
			exit(-1);
		}
		
	}

	bool next(T &out) {
		if (i < cnt) {
			while (avail == 0) {
				avail = (*rdfo) & 0x7fffffff;
				if (do_busy_wait)
					nano_sleep(BUSY_WAIT_NANO_SLEEP_TIME);
				else
					usleep(BUSY_WAIT_MICRO_SLEEP_TIME);
			}
			--avail;
			++i;
			out = *rdfd;
			return true;
		} else {
			return false;
		}
	}
private:
	IODev axi_ctrl;
	unsigned cnt;
	bool do_busy_wait;
	// Receive Data FIFO Occupancy (RDFO)
	volatile unsigned *rdfo;
	// Receive Data FIFO Data (RDFD)
	volatile T *rdfd;

	unsigned avail = 0;
	unsigned i = 0;
};


// continuity correction, see Broadie, Glasserman, Kou (1997)
#define BARRIER_HIT_CORRECTION 0.5826


struct HestonParamsHW {
	// call option
	float log_spot_price;
	float reversion_rate_TIMES_step_size;
	float long_term_avg_vola;
	float vol_of_vol_TIMES_sqrt_step_size;
	float double_riskless_rate; // = 2 * riskless_rate
	float vola_0;
    	// both knockout
	float log_lower_barrier_value;
	float log_upper_barrier_value;
	// simulation params
	uint32_t step_cnt;
	float step_size; // = time_to_maturity / step_cnt
	float half_step_size; // = step_size / 2
	float sqrt_step_size; // = sqrt(step_size)
	// = BARRIER_HIT_CORRECTION * sqrt_step_size
	float barrier_correction_factor; 
	uint32_t path_cnt;
};


Json::Value read_params(char *filename) {
	std::ifstream file(filename);
	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(file, root)) {
		std::cerr << "Failed to parse parameter file" << std::endl
				<< reader.getFormattedErrorMessages();
		exit(-1);
	}
	return root;
}


float heston_sl_hw(
		// call option
		float spot_price,
		float reversion_rate,
		float long_term_avg_vola,
		float vol_of_vol,
		float riskless_rate,
		float vola_0,
		float correlation,
		float time_to_maturity,
		float strike_price,
		// both knowckout
		float lower_barrier_value,
		float upper_barrier_value,
		// simulation params
		uint32_t step_cnt,
		uint32_t path_cnt)
{
	// get device pointers
	IODev axi_heston(AXI_HESTON_BASE_ADDR, AXI_HESTON_SIZE);
	volatile unsigned &acc_ctrl = *((unsigned*)axi_heston.get_dev_ptr(0x00));

	// make sure heston accelerator is not running
	bool acc_idle = acc_ctrl & 0x4;
	if (!acc_idle) {
		std::cerr << "Heston accelerator is already running." << std::endl;
		exit(EXIT_FAILURE);
	}

	// make sure random number generator is running
	IODev axi_rng(AXI_RNG_BASE_ADDR, AXI_RNG_SIZE);
	bool rng_idle = *(unsigned*)(axi_rng.get_dev_ptr(0x00)) & 0x4;
	if (rng_idle) {
		std::cerr << "Start the Random Number generator before running"
			<< " this application" << std::endl;
		exit(EXIT_FAILURE);
	}
	
	// define heston hw parameters
	float step_size = time_to_maturity / step_cnt;
	float sqrt_step_size = std::sqrt(step_size);
	HestonParamsHW params_hw = {
		std::log(spot_price),
		reversion_rate * step_size,
		long_term_avg_vola,
		vol_of_vol * sqrt_step_size,
		2 * riskless_rate,
		vola_0,
		std::log(lower_barrier_value),
		std::log(upper_barrier_value),
		step_cnt,
		step_size,
		step_size / 2,
		sqrt_step_size,
		(float) BARRIER_HIT_CORRECTION * sqrt_step_size,
		path_cnt};

	// send parameters to accelerator, 64 bit aligned
	for (unsigned i = 0x14, j = 0; i <= 0x7c; i = i + 8, ++j) {
		*((unsigned*)axi_heston.get_dev_ptr(i)) = 
				*((unsigned*)&params_hw + j);
	}

	// start heston accelerator
	acc_ctrl = 1;
	
	// setup read iterator
	read_iterator<float> read_it(path_cnt);

	// calculate result
	double result = 0;
	float price;
	while (read_it.next(price)) {
		result += std::max(0.f, std::exp(price) - strike_price);
	}
	result *= std::exp(-riskless_rate * time_to_maturity) / path_cnt;
	return result;
}



void array_stream_cpu(double a, double b, double *out, unsigned out_len) {
	for (unsigned i = 0; i < out_len; ++i) {
		out[i] = 0.2 * 2.5 + i;
	}
}


int main(int argc, char *argv[])
{
	if (argc != 2) {
		std::cout << "Usage: " << argv[0] << " params.json" << std::endl;
		return -1;
	}
	Json::Value params = read_params(argv[1]);

	// heston params
	auto heston = params["heston"];
	float spot_price = heston["spot_price"].asFloat();
	float reversion_rate = heston["reversion_rate"].asFloat();
	float long_term_avg_vola = heston["long_term_avg_vola"].asFloat();
	float vol_of_vol = heston["vol_of_vol"].asFloat();
	float riskless_rate = heston["riskless_rate"].asFloat();
	float vola_0 = heston["vola_0"].asFloat();
	float correlation = heston["correlation"].asFloat();
	float time_to_maturity = heston["time_to_maturity"].asFloat();
	float strike_price = heston["strike_price"].asFloat();
	// both knowckout
	auto barrier = params["barrier values"];
	float lower_barrier_value = barrier["lower"].asFloat();
	float upper_barrier_value = barrier["upper"].asFloat();
	// simulation params
	auto simulation = params["simulation"];
	uint32_t step_cnt = simulation["step_cnt"].asUInt();
	uint32_t path_cnt = simulation["path_cnt"].asUInt();

	// benchmark
	auto start_cpu = std::chrono::steady_clock::now();
//	float result_cpu = heston_sl_cpu(a, b, out_cpu, cnt);
	auto start_acc = std::chrono::steady_clock::now();
	float result_acc = heston_sl_hw(spot_price, reversion_rate,
		long_term_avg_vola, vol_of_vol, riskless_rate, vola_0,
		correlation, time_to_maturity, strike_price, lower_barrier_value,
		upper_barrier_value, step_cnt, path_cnt);
	auto end = std::chrono::steady_clock::now();

	// reference values
	auto ref = params["reference"];
	float ref_price = ref["price"].asFloat();
	float ref_price_precision = ref["precision"].asFloat();

	std::cout << "REF: result = " << ref_price << std::endl;
//	std::cout << "CPU: result = " << result_cpu << std::endl;
	std::cout << "CPU: "; print_duration(start_cpu, start_acc, path_cnt);
	std::cout << "ACC: result = " << result_acc << std::endl;
	std::cout << "ACC: "; print_duration(start_acc, end, path_cnt);

	std::cout << "done" << std::endl;
	return 0;
}


