﻿#ifndef AUTOREG_HH
#define AUTOREG_HH

#include <algorithm>             // for min, any_of, copy_n, for_each, generate
#include <cassert>               // for assert
#include <chrono>                // for duration, steady_clock, steady_clock...
#include <cmath>                 // for isnan
#include <cstdlib>               // for abs
#include <functional>            // for bind
#include <iostream>              // for operator<<, cerr, endl
#include <fstream>               // for ofstream
#include <random>                // for mt19937, normal_distribution
#include <stdexcept>             // for runtime_error
#include <string>
#include <blitz/array.h>         // for Array, Range, shape, any
#include "sysv.hh"               // for sysv
#include "types.hh"              // for size3, ACF, AR_coefs, Zeta, Array2D
#include "voodoo.hh"             // for generate_AC_matrix
#include "parallel_mt.hh"
#include <omp.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
/// @file
/// File with subroutines for AR model, Yule-Walker equations
/// and some others.

namespace autoreg {

	template<class T>
	ACF<T>
	approx_acf(T alpha, T beta, T gamm, const Vec3<T>& delta, const size3& acf_size) {
		ACF<T> acf(acf_size);
		blitz::firstIndex t;
		blitz::secondIndex x;
		blitz::thirdIndex y;
		acf = gamm
			* blitz::exp(-alpha * (t*delta[0] + x*delta[1] + y*delta[2]))
//	 		* blitz::cos(beta * (t*delta[0] + x*delta[1] + y*delta[2]));
	 		* blitz::cos(beta * t * delta[0])
	 		* blitz::cos(beta * x * delta[1])
	 		* blitz::cos(beta * y * delta[2]);
		return acf;
	}

	template<class T>
	T white_noise_variance(const AR_coefs<T>& ar_coefs, const ACF<T>& acf) {
		return acf(0,0,0) - blitz::sum(ar_coefs * acf);
	}

	template<class T>
	T ACF_variance(const ACF<T>& acf) {
		return acf(0,0,0);
	}

	/// Удаление участков разгона из реализации.
	template<class T>
	Zeta<T>
	trim_zeta(const Zeta<T>& zeta2, const size3& zsize) {
		using blitz::Range;
		using blitz::toEnd;
		size3 zsize2 = zeta2.shape();
		return zeta2(
			Range(zsize2(0) - zsize(0), toEnd),
			Range(zsize2(1) - zsize(1), toEnd),
			Range(zsize2(2) - zsize(2), toEnd)
		);
	}

	template<class T>
	bool is_stationary(AR_coefs<T>& phi) {
		return !blitz::any(blitz::abs(phi) > T(1));
	}

	template<class T>
	AR_coefs<T>
	compute_AR_coefs(const ACF<T>& acf) {
		using blitz::Range;
		using blitz::toEnd;
		const int m = acf.numElements()-1;
		Array2D<T> acm = generate_AC_matrix(acf);
		//{ std::ofstream out("acm"); out << acm; }

		/**
		eliminate the first equation and move the first column of the remaining
		matrix to the right-hand side of the system
		*/
		Array1D<T> rhs(m);
		rhs = acm(Range(1, toEnd), 0);
		//{ std::ofstream out("rhs"); out << rhs; }

		// lhs is the autocovariance matrix without first
		// column and row
		Array2D<T> lhs(blitz::shape(m,m));
		lhs = acm(Range(1, toEnd), Range(1, toEnd));
		//{ std::ofstream out("lhs"); out << lhs; }

		assert(lhs.extent(0) == m);
		assert(lhs.extent(1) == m);
		assert(rhs.extent(0) == m);
		sysv<T>('U', m, 1, lhs.data(), m, rhs.data(), m);
		AR_coefs<T> phi(acf.shape());
		assert(phi.numElements() == rhs.numElements() + 1);
		phi(0,0,0) = 0;
		std::copy_n(rhs.data(), rhs.numElements(), phi.data()+1);
		//{ std::ofstream out("ar_coefs"); out << phi; }
		if (!is_stationary(phi)) {
			std::cerr << "phi.shape() = " << phi.shape() << std::endl;
			std::for_each(
				phi.begin(),
				phi.end(),
				[] (T val) {
					if (std::abs(val) > T(1)) {
						std::cerr << val << std::endl;
					}
				}
			);
			throw std::runtime_error("AR process is not stationary, i.e. |phi| > 1");
		}
		return phi;
	}

	template<class T>
	bool
	isnan(T rhs) noexcept {
		return std::isnan(rhs);
	}

	/// Генерация белого шума по алгоритму Вихря Мерсенна и
	/// преобразование его к нормальному распределению по алгоритму Бокса-Мюллера.
	template<class T>
	Zeta<T>
	generate_white_noise(const size3& size, const T variance) {
          	if (variance < T(0)) {
			throw std::runtime_error("variance is less than zero");
		}

		// инициализация генератора
		//std::mt19937 generator;
		//#if !defined(DISABLE_RANDOM_SEED)
		//generator.seed(std::chrono::steady_clock::now().time_since_epoch().count());
		//#endif
		std::normal_distribution<T> normal(T(0), std::sqrt(variance));
                parallel_mt* gens[8];

		// генерация и проверка
		Zeta<T> eps(size);
                size_t step = (size(0)*size(1)*size(2) + 7) / 8;

                omp_set_num_threads(8);
		#pragma omp parallel
		{
                #pragma omp for 
                for (int i = 0; i < 8; i++)
                {
   	       	 std::string path = "./MT/out";
                  std::ifstream in;
                  char filename[2];

		  path = "./MT/out";
                  sprintf(filename,"%d",i);
                  path += filename;
                  std::clog << path << '\n';

                  in.open(path, std::fstream::in);
                  mt_config gen;
                  in >> gen;
                  in.close();
                  gens[i] = new parallel_mt(gen);

                  auto b = std::next(std::begin(eps), step*i);
                  auto e = std::next(b, step);
                  if (i*(step+1) > size(0)*size(1)*size(2)) e = std::end(eps);
		  std::generate(b, e, std::bind(normal, *gens[i]));
		  if (std::any_of(b, e, &::autoreg::isnan<T>)) {
		  	throw std::runtime_error("white noise generator produced some NaNs");
		  }
                }
		}
                
                std::clog << "WN GENERATED!\n";
		return eps;
	}

	/// Генерация отдельных частей реализации волновой поверхности.
	template<class T>
	void generate_zeta(const AR_coefs<T>& phi, Zeta<T>& zeta) {
		const size3 fsize = phi.shape();
		const size3 zsize = zeta.shape();
		const int t1 = zsize[0];
		const int x1 = zsize[1];
		const int y1 = zsize[2];
		const int stept = 30;
		const int stepx = 4;
		const int stepy = 4;
		std::clog << "BRUH..." << " " << t1 << " " << x1 << " " << y1 << '\n';
		int*** prog = new int** [(t1 + stept - 1)/stept];
		std::clog << "LETS START" << '\n';
		for (int t=0; t<(t1 + stept - 1)/stept; t++) {
			prog[t] = new int* [(x1 + stepx - 1)/stepx];
			for (int x=0; x<(x1 + stepx - 1)/stepx; x++){
				prog[t][x] = new int [(y1 + stepy -1)/stepy]; 
				for (int y=0; y<(y1 + stepy -1)/stepy; y++)
					{
					prog[t][x][y] = 0;
					if (t==0 && x!=0 && y!=0) prog[t][x][y] += 4;
 				        if (t==0 && x==0 && y!=0) prog[t][x][y] += 6;
					if (t==0 && x!=0 && y==0) prog[t][x][y] += 6;
					if (t!=0 && x==0 && y==0) prog[t][x][y] += 6;
					if (t!=0 && x==0 && y!=0) prog[t][x][y] += 4;
					if (t!=0 && x!=0 && y==0) prog[t][x][y] += 4;

				}
			}
		}
		std::queue<std::tuple<int, int, int>> tasks;
		tasks.push(std::make_tuple(0, 0, 0));
		int counter = 0;
		const int len = ((t1 + stept - 1)/stept)*((x1 + stepx - 1)/stepx)*((y1 + stepy - 1)/stepy);
		omp_lock_t lock;
		omp_init_lock(&lock);
                omp_lock_t clock;
                omp_init_lock(&clock);
		std::mutex cv_m;
		std::condition_variable cv;
		#pragma omp parallel num_threads(8)
		{
		std::unique_lock<std::mutex> tlock{cv_m};
		cv.wait(tlock, [&] () {
		while (!tasks.empty()) 
			{
			std::tuple<int, int, int> task;
			//omp_set_lock(&lock);
			task = tasks.front();
			tasks.pop();
			//omp_unset_lock(&lock);
			tlock.unlock();
                        int t = std::get<0>(task);
                        int x = std::get<1>(task);
			int y = std::get<2>(task);
                        //omp_set_lock(&clock);
                        //std::clog <<  "GET (" << t << "," << x << "," << y << ") T:" << omp_get_thread_num() << '\n';
                        //omp_unset_lock(&clock);
			int tb = t*stept;
			int te = std::min(t*(stept+1), t1);
                        int xb = x*stept;
                        int xe = std::min(x*(stepx+1), x1);
                        int yb = y*stept;
                        int ye = std::min(y*(stept+1), y1);


			for (int tc = tb; tc < te; tc++)
				for (int xc = xb; xc < xe; xc++)
					for (int yc = yb; yc < ye; yc++)
					{
					const int m1 = std::min(tc+1, fsize[0]);
					const int m2 = std::min(xc+1, fsize[1]);
					const int m3 = std::min(yc+1, fsize[2]);
					T sum = 0;
					//std::clog << t << " " << x << " " << y << '\n';
					for (int k=tb; k<m1; k++)
						for (int i=xb; i<m2; i++)
							for (int j=yb; j<m3; j++)
								sum += phi(k, i, j)*zeta(tc-k, xc-i, yc-j);
					zeta(tc, xc, yc) += sum;
					}
			tlock.lock();
			counter++;
			
			for (int k=0; k<2; k++)
				for (int i=0; i<2; i++)
					for (int j=0; j<2; j++)
						{
							if (k==0 && i==0 && j==0) continue;
							const int tc = std::min(t+k, t1);
							const int xc = std::min(x+i, x1);
							const int yc = std::min(y+j, y1);
							if ((t+k)*stept >= t1) continue;
							if ((x+i)*stepx >= x1) continue;
							if ((y+j)*stepy >= y1) continue;
							//#pragma omp atomic
							//omp_set_lock(&clock);
							prog[t+k][x+i][y+j]++;
							//std::clog << tc << '-' << xc << '-' << yc << ' ' << prog[tc][xc][yc] << " T" << omp_get_thread_num() <<  '\n';
							//omp_unset_lock(&clock);
							if (prog[tc][xc][yc] == 7) 
							{
							//omp_set_lock(&lock);
							tasks.push(std::make_tuple(tc, xc, yc));
							//omp_unset_lock(&lock);
							cv.notify_one();
							}
						}

			//omp_set_lock(&clock);
			//std::clog << "DONE (" << t << "," << x << "," << y << ") " << counter << '|' << len << ' ' <<  tasks.size() << ' ' << omp_get_thread_num() << '\n';
			//omp_unset_lock(&clock);
			//tlock.unlock();
			}
		   cv.notify_all();
		   //tlock.unlock();
		   return counter >= len;
		   }
		 );
		}

	}

	template<class T, int N>
	T mean(const blitz::Array<T,N>& rhs) {
		return blitz::sum(rhs) / rhs.numElements();
	}

	template<class T, int N>
	T variance(const blitz::Array<T,N>& rhs) {
		assert(rhs.numElements() > 0);
		const T m = mean(rhs);
		return blitz::sum(blitz::pow(rhs-m, 2)) / (rhs.numElements() - 1);
	}

}

#endif // AUTOREG_HH
