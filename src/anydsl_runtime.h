#ifndef ANYDSL_RUNTIME_H
#define ANYDSL_RUNTIME_H

#include <stdint.h>
#include <stdlib.h>
#include <mpi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANYDSL_DEVICE(p, d) ((p) | ((d) << 4))

enum {
    ANYDSL_HOST = 0,
    ANYDSL_CUDA = 1,
    ANYDSL_OPENCL = 2,
    ANYDSL_HSA = 3
};

void anydsl_info(void);

void* anydsl_alloc(int32_t, int64_t);
void* anydsl_alloc_host(int32_t, int64_t);
void* anydsl_alloc_unified(int32_t, int64_t);
void* anydsl_get_device_ptr(int32_t, void*);
void  anydsl_release(int32_t, void*);
void  anydsl_release_host(int32_t, void*);

void anydsl_copy(int32_t, const void*, int64_t, int32_t, void*, int64_t, int64_t);

void anydsl_launch_kernel(int32_t,
                          const char*, const char*,
                          const uint32_t*, const uint32_t*,
                          void**, const uint32_t*, const uint8_t*,
                          uint32_t);
void anydsl_synchronize(int32_t);

void anydsl_random_seed(uint32_t);
float    anydsl_random_val_f32();
uint64_t anydsl_random_val_u64();

uint64_t anydsl_get_micro_time();
uint64_t anydsl_get_kernel_time();

int32_t anydsl_isinff(float);
int32_t anydsl_isnanf(float);
int32_t anydsl_isfinitef(float);
int32_t anydsl_isinf(double);
int32_t anydsl_isnan(double);
int32_t anydsl_isfinite(double);

void anydsl_print_i16(int16_t);
void anydsl_print_i32(int32_t);
void anydsl_print_i64(int64_t);
void anydsl_print_f32(float);
void anydsl_print_f64(double);
void anydsl_print_char(char);
void anydsl_print_string(char*);

void* anydsl_aligned_malloc(size_t, size_t);
void anydsl_aligned_free(void*);

void anydsl_parallel_for(int32_t, int32_t, int32_t, void*, void*);
int32_t anydsl_spawn_thread(void*, void*);
void anydsl_sync_thread(int32_t);

struct Closure {
    void (*fn)(uint64_t);
    uint64_t payload;
};

int32_t anydsl_create_graph();
int32_t anydsl_create_task(int32_t, Closure);
void    anydsl_create_edge(int32_t, int32_t);
void    anydsl_execute_graph(int32_t, int32_t);

#ifdef RUNTIME_ENABLE_JIT
void  anydsl_link(const char*);
int32_t anydsl_compile(const char*, uint32_t, uint32_t);
void *anydsl_lookup_function(int32_t, const char*);
#endif

//COMMUNICATOR
//TODO adjust
int anydsl_comm_init();
MPI_Op anydsl_comm_get_max();
MPI_Op anydsl_comm_get_sum();
MPI_Datatype anydsl_comm_get_int();
MPI_Datatype anydsl_comm_get_double();
MPI_Datatype anydsl_comm_get_char();
MPI_Datatype anydsl_comm_get_byte();
MPI_Comm anydsl_comm_get_world();
MPI_Status* anydsl_comm_get_status_ignore();

const auto& anydsl_comm_initialized = MPI_Initialized;

/*
int MPI_init();
MPI_Op get_mpi_max();
MPI_Op get_mpi_sum();
MPI_Datatype get_mpi_int();
MPI_Datatype get_mpi_double();
MPI_Datatype get_mpi_char();
MPI_Datatype get_mpi_byte();
MPI_Comm get_mpi_comm_world();
MPI_Status* get_mpi_status_ignore();
*/

#ifdef __cplusplus
}
#include "anydsl_runtime.hpp"
#endif

#endif
