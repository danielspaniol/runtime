#include "hsa_platform.h"
#include "runtime.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#ifdef RUNTIME_ENABLE_JIT
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#endif

#define CHECK_HSA(err, name)  check_hsa_error(err, name, __FILE__, __LINE__)

inline void check_hsa_error(hsa_status_t err, const char* name, const char* file, const int line) {
    if (err != HSA_STATUS_SUCCESS) {
        const char* status_string;
        hsa_status_t ret = hsa_status_string(err, &status_string);
        if (ret != HSA_STATUS_SUCCESS)
            info("hsa_status_string failed: %", ret);
        error("HSA API function % (%) [file %, line %]: %", name, err, file, line, status_string);
    }
}

std::string get_device_profile_str(hsa_profile_t profile) {
    #define HSA_PROFILE_TYPE(TYPE) case TYPE: return #TYPE;
    switch (profile) {
        HSA_PROFILE_TYPE(HSA_PROFILE_BASE)
        HSA_PROFILE_TYPE(HSA_PROFILE_FULL)
        default: return "unknown HSA profile";
    }
}

std::string get_device_type_str(hsa_device_type_t device_type) {
    #define HSA_DEVICE_TYPE(TYPE) case TYPE: return #TYPE;
    switch (device_type) {
        HSA_DEVICE_TYPE(HSA_DEVICE_TYPE_CPU)
        HSA_DEVICE_TYPE(HSA_DEVICE_TYPE_GPU)
        HSA_DEVICE_TYPE(HSA_DEVICE_TYPE_DSP)
        default: return "unknown HSA device type";
    }
}

std::string get_region_segment_str(hsa_region_segment_t region_segment) {
    #define HSA_REGION_SEGMENT(TYPE) case TYPE: return #TYPE;
    switch (region_segment) {
        HSA_REGION_SEGMENT(HSA_REGION_SEGMENT_GLOBAL)
        HSA_REGION_SEGMENT(HSA_REGION_SEGMENT_READONLY)
        HSA_REGION_SEGMENT(HSA_REGION_SEGMENT_PRIVATE)
        HSA_REGION_SEGMENT(HSA_REGION_SEGMENT_GROUP)
        HSA_REGION_SEGMENT(HSA_REGION_SEGMENT_KERNARG)
        default: return "unknown HSA region segment";
    }
}

hsa_status_t HSAPlatform::iterate_agents_callback(hsa_agent_t agent, void* data) {
    auto devices_ = static_cast<std::vector<DeviceData>*>(data);
    hsa_status_t status;

    char name[64] = { 0 };
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("  (%) Device Name: %", devices_->size(), name);
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_VENDOR_NAME, name);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("      Device Vendor: %", name);

    hsa_profile_t profile;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_PROFILE, &profile);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("      Device profile: %", get_device_profile_str(profile));

    hsa_default_float_rounding_mode_t float_mode;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEFAULT_FLOAT_ROUNDING_MODE, &float_mode);
    CHECK_HSA(status, "hsa_agent_get_info()");

    hsa_isa_t isa;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_ISA, &isa);
    CHECK_HSA(status, "hsa_agent_get_info()");
    uint32_t name_length;
    status = hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME_LENGTH, &name_length);
    status = hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME, &name);
    debug("      Device ISA: %", name);
    std::string name_string = name;
    auto dash_pos = name_string.rfind('-');
    std::string isa_name = dash_pos != std::string::npos ? name_string.substr(dash_pos + 1) : "";

    hsa_device_type_t device_type;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &device_type);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("      Device Type: %", get_device_type_str(device_type));

    uint16_t version_major, version_minor;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_VERSION_MAJOR, &version_major);
    CHECK_HSA(status, "hsa_agent_get_info()");
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_VERSION_MINOR, &version_minor);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("      HSA Runtime Version: %.%", version_major, version_minor);

    uint32_t queue_size = 0;
    status = hsa_agent_get_info(agent, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &queue_size);
    CHECK_HSA(status, "hsa_agent_get_info()");
    debug("      Queue Size: %", queue_size);

    hsa_queue_t* queue = nullptr;
    if (queue_size > 0) {
        status = hsa_queue_create(agent, queue_size, HSA_QUEUE_TYPE_SINGLE, NULL, NULL, UINT32_MAX, UINT32_MAX, &queue);
        CHECK_HSA(status, "hsa_queue_create()");

        status = hsa_amd_profiling_set_profiler_enabled(queue, 1);
        CHECK_HSA(status, "hsa_amd_profiling_set_profiler_enabled()");
    }

    hsa_signal_t signal;
    status = hsa_signal_create(0, 0, NULL, &signal);
    CHECK_HSA(status, "hsa_signal_create()");

    auto dev = devices_->size();
    devices_->resize(dev + 1);
    DeviceData* device = &(*devices_)[dev];
    device->agent = agent;
    device->profile = profile;
    device->float_mode = float_mode;
    device->isa = isa_name;
    device->queue = queue;
    device->signal = signal;
    device->kernarg_region.handle = { 0 };
    device->finegrained_region.handle = { 0 };
    device->coarsegrained_region.handle = { 0 };

    status = hsa_agent_iterate_regions(agent, iterate_regions_callback, device);
    CHECK_HSA(status, "hsa_agent_iterate_regions()");

    return HSA_STATUS_SUCCESS;
}

hsa_status_t HSAPlatform::iterate_regions_callback(hsa_region_t region, void* data) {
    DeviceData* device = static_cast<DeviceData*>(data);
    hsa_status_t status;

    hsa_region_segment_t segment;
    status = hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment);
    CHECK_HSA(status, "hsa_region_get_info()");
    debug("      Region Segment: %", get_region_segment_str(segment));

    hsa_region_global_flag_t flags;
    status = hsa_region_get_info(region, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
    CHECK_HSA(status, "hsa_region_get_info()");

    std::string global_flags;
    if (flags & HSA_REGION_GLOBAL_FLAG_KERNARG) {
        global_flags += "HSA_REGION_GLOBAL_FLAG_KERNARG ";
        device->kernarg_region = region;
    }
    if (flags & HSA_REGION_GLOBAL_FLAG_FINE_GRAINED) {
        global_flags += "HSA_REGION_GLOBAL_FLAG_FINE_GRAINED ";
        device->finegrained_region = region;
    }
    if (flags & HSA_REGION_GLOBAL_FLAG_COARSE_GRAINED) {
        global_flags += "HSA_REGION_GLOBAL_FLAG_COARSE_GRAINED ";
        device->coarsegrained_region = region;
    }
    debug("      Region Global Flags: %", global_flags);

    bool runtime_alloc_allowed;
    status = hsa_region_get_info(region, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &runtime_alloc_allowed);
    CHECK_HSA(status, "hsa_region_get_info()");
    debug("      Region Runtime Alloc Allowed: %", runtime_alloc_allowed);

    return HSA_STATUS_SUCCESS;
}

HSAPlatform::HSAPlatform(Runtime* runtime)
    : Platform(runtime)
{
    hsa_status_t status = hsa_init();
    CHECK_HSA(status, "hsa_init()");

    uint16_t version_major, version_minor;
    status = hsa_system_get_info(HSA_SYSTEM_INFO_VERSION_MAJOR, &version_major);
    CHECK_HSA(status, "hsa_system_get_info()");
    status = hsa_system_get_info(HSA_SYSTEM_INFO_VERSION_MINOR, &version_minor);
    CHECK_HSA(status, "hsa_system_get_info()");
    debug("HSA System Runtime Version: %.%", version_major, version_minor);

    status = hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY, &frequency_);
    CHECK_HSA(status, "hsa_system_get_info()");

    status = hsa_iterate_agents(iterate_agents_callback, &devices_);
    CHECK_HSA(status, "hsa_iterate_agents()");
}

HSAPlatform::~HSAPlatform() {
    hsa_status_t status;

    for (size_t i = 0; i < devices_.size(); i++) {
        for (auto& it : devices_[i].programs) {
            status = hsa_executable_destroy(it.second);
            CHECK_HSA(status, "hsa_executable_destroy()");
        }
        if (auto queue = devices_[i].queue) {
            status = hsa_queue_destroy(queue);
            CHECK_HSA(status, "hsa_queue_destroy()");
        }
        status = hsa_signal_destroy(devices_[i].signal);
        CHECK_HSA(status, "hsa_signal_destroy()");
    }

    hsa_shut_down();
}

void* HSAPlatform::alloc_hsa(int64_t size, hsa_region_t region) {
    if (!size)
        return nullptr;

    char* mem;
    hsa_status_t status = hsa_memory_allocate(region, size, (void**) &mem);
    CHECK_HSA(status, "hsa_memory_allocate()");

    return (void*)mem;
}

void HSAPlatform::release(DeviceId, void* ptr) {
    hsa_status_t status = hsa_memory_free(ptr);
    CHECK_HSA(status, "hsa_memory_free()");
}

extern std::atomic<uint64_t> anydsl_kernel_time;

void HSAPlatform::launch_kernel(DeviceId dev,
                                const char* file, const char* name,
                                const uint32_t* grid, const uint32_t* block,
                                void** args, const uint32_t* sizes, const KernelArgType*,
                                uint32_t num_args) {
    auto queue = devices_[dev].queue;
    if (!queue)
        error("The selected HSA device '%' cannot execute kernels", dev);

    uint64_t kernel;
    uint32_t kernarg_segment_size;
    uint32_t group_segment_size;
    uint32_t private_segment_size;
    std::tie(kernel, kernarg_segment_size, group_segment_size, private_segment_size) = load_kernel(dev, file, name);

    // set up arguments
    hsa_status_t status;
    void* kernarg_address = nullptr;
    status = hsa_memory_allocate(devices_[dev].kernarg_region, kernarg_segment_size, &kernarg_address);
    CHECK_HSA(status, "hsa_memory_allocate()");
    size_t offset = 0;
    auto align_address = [] (size_t base, size_t align) {
        if (align > 8)
            align = 8;
        return ((base + align - 1) / align) * align;
    };
    for (uint32_t i = 0; i < num_args; i++) {
        // align base address for next kernel argument
        offset = align_address(offset, sizes[i]);
        std::memcpy((void*)((char*)kernarg_address + offset), args[i], sizes[i]);
        offset += sizes[i];
    }
    if (offset != kernarg_segment_size)
        debug("HSA kernarg segment size for kernel '%' differs from argument size: % vs. %", name, kernarg_segment_size, offset);

    auto signal = devices_[dev].signal;
    hsa_signal_add_relaxed(signal, 1);

    hsa_signal_t launch_signal;
    if (runtime_->profiling_enabled()) {
        status = hsa_signal_create(1, 0, NULL, &launch_signal);
        CHECK_HSA(status, "hsa_signal_create()");
    } else
        launch_signal = signal;

    // construct aql packet
    hsa_kernel_dispatch_packet_t aql;
    std::memset(&aql, 0, sizeof(aql));

    aql.header = (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_ACQUIRE_FENCE_SCOPE) |
                 (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_RELEASE_FENCE_SCOPE) |
                 (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE);
    aql.setup = 3 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    aql.workgroup_size_x = (uint16_t)block[0];
    aql.workgroup_size_y = (uint16_t)block[1];
    aql.workgroup_size_z = (uint16_t)block[2];
    aql.grid_size_x = grid[0];
    aql.grid_size_y = grid[1];
    aql.grid_size_z = grid[2];
    aql.completion_signal = launch_signal;
    aql.kernel_object = kernel;
    aql.kernarg_address = kernarg_address;
    aql.private_segment_size = private_segment_size;
    aql.group_segment_size = group_segment_size;

    // write to command queue
    const uint64_t index = hsa_queue_load_write_index_relaxed(queue);
    const uint32_t queue_mask = queue->size - 1;
    ((hsa_kernel_dispatch_packet_t*)(queue->base_address))[index & queue_mask] = aql;
    hsa_queue_store_write_index_relaxed(queue, index + 1);
    hsa_signal_store_relaxed(queue->doorbell_signal, index);

    if (runtime_->profiling_enabled())
        std::thread ([=] {
            hsa_signal_value_t completion = hsa_signal_wait_relaxed(launch_signal, HSA_SIGNAL_CONDITION_EQ, 0, UINT64_MAX, HSA_WAIT_STATE_ACTIVE);
            if (completion != 0)
                debug("HSA launch_signal completion failed: %", completion);

            hsa_amd_profiling_dispatch_time_t dispatch_times = { 0, 0 };
            hsa_status_t status = hsa_amd_profiling_get_dispatch_time(devices_[dev].agent, launch_signal, &dispatch_times);
            CHECK_HSA(status, "hsa_amd_profiling_get_dispatch_time()");

            anydsl_kernel_time.fetch_add(1000000.0 * double(dispatch_times.end - dispatch_times.start) / double(frequency_));
            hsa_signal_subtract_relaxed(signal, 1);

            status = hsa_signal_destroy(launch_signal);
            CHECK_HSA(status, "hsa_signal_destroy()");
        }).detach();
}

void HSAPlatform::synchronize(DeviceId dev) {
    auto signal = devices_[dev].signal;
    hsa_signal_value_t completion = hsa_signal_wait_relaxed(signal, HSA_SIGNAL_CONDITION_EQ, 0, UINT64_MAX, HSA_WAIT_STATE_ACTIVE);
    if (completion != 0)
        debug("HSA signal completion failed: %", completion);
}

void HSAPlatform::copy(const void* src, int64_t offset_src, void* dst, int64_t offset_dst, int64_t size) {
    hsa_status_t status = hsa_memory_copy((char*)dst + offset_dst, (char*)src + offset_src, size);
    CHECK_HSA(status, "hsa_memory_copy()");
}

void HSAPlatform::register_file(const std::string& filename, const std::string& program_string) {
    files_[filename] = program_string;
}

std::string HSAPlatform::load_file(const std::string& filename) const {
    auto file_it = files_.find(filename);
    if (file_it != files_.end())
        return file_it->second;

    std::ifstream src_file(filename);
    if (!src_file.is_open())
        error("Can't open source file '%'", filename);

    return std::string(std::istreambuf_iterator<char>(src_file), (std::istreambuf_iterator<char>()));
}

void HSAPlatform::store_file(const std::string& filename, const std::string& str) const {
    std::ofstream dst_file(filename);
    if (!dst_file)
        error("Can't open destination file '%'", filename);
    dst_file << str;
    dst_file.close();
}

std::tuple<uint64_t, uint32_t, uint32_t, uint32_t> HSAPlatform::load_kernel(DeviceId dev, const std::string& filename, const std::string& kernelname) {
    auto& hsa_dev = devices_[dev];
    hsa_status_t status;

    hsa_dev.lock();

    hsa_executable_t executable = { 0 };
    auto& prog_cache = hsa_dev.programs;
    auto prog_it = prog_cache.find(filename);
    if (prog_it == prog_cache.end()) {
        hsa_dev.unlock();

        // find the file extension
        auto ext_pos = filename.rfind('.');
        std::string ext = ext_pos != std::string::npos ? filename.substr(ext_pos + 1) : "";
        if (ext != "gcn" && ext != "amdgpu")
            error("Incorrect extension for kernel file '%' (should be '.gcn' or '.amdgpu')", filename);

        std::string gcn;
        if (ext == "gcn" && (std::ifstream(filename).good() || files_.count(filename))) {
            gcn = load_file(filename);
        } else if (ext == "amdgpu" && (std::ifstream(filename).good() || files_.count(filename))) {
            gcn = compile_gcn(dev, filename, load_file(filename));
        } else {
            error("Could not find kernel file '%'", filename);
        }

        hsa_code_object_reader_t reader;
        status = hsa_code_object_reader_create_from_memory(gcn.data(), gcn.size(), &reader);
        CHECK_HSA(status, "hsa_code_object_reader_create_from_file()");

        debug("Compiling '%' on HSA device %", filename, dev);

        status = hsa_executable_create_alt(HSA_PROFILE_FULL /* hsa_dev.profile */, hsa_dev.float_mode, NULL, &executable);
        CHECK_HSA(status, "hsa_executable_create_alt()");

        // TODO
        //hsa_loaded_code_object_t program_code_object;
        //status = hsa_executable_load_program_code_object(executable, reader, "", &program_code_object);
        //CHECK_HSA(status, "hsa_executable_load_program_code_object()");
        // -> hsa_executable_global_variable_define()
        // -> hsa_executable_agent_variable_define()
        // -> hsa_executable_readonly_variable_define()

        hsa_loaded_code_object_t agent_code_object;
        status = hsa_executable_load_agent_code_object(executable, hsa_dev.agent, reader, NULL, &agent_code_object);
        CHECK_HSA(status, "hsa_executable_load_agent_code_object()");

        status = hsa_executable_freeze(executable, NULL);
        CHECK_HSA(status, "hsa_executable_freeze()");

        status = hsa_code_object_reader_destroy(reader);
        CHECK_HSA(status, "hsa_code_object_reader_destroy()");

        uint32_t validated;
        status = hsa_executable_validate(executable, &validated);
        CHECK_HSA(status, "hsa_executable_validate()");

        if (validated != 0)
            debug("HSA executable validation failed: %", validated);

        hsa_dev.lock();
        prog_cache[filename] = executable;
    } else {
        executable = prog_it->second;
    }

    // checks that the kernel exists
    auto& kernel_cache = hsa_dev.kernels;
    auto& kernel_map = kernel_cache[executable.handle];
    auto kernel_it = kernel_map.find(kernelname);
    uint64_t kernel = 0;
    uint32_t kernarg_segment_size = 0;
    uint32_t group_segment_size = 0;
    uint32_t private_segment_size = 0;
    if (kernel_it == kernel_map.end()) {
        hsa_dev.unlock();

        hsa_executable_symbol_t kernel_symbol = { 0 };
        // DEPRECATED: use hsa_executable_get_symbol_by_linker_name if available
        status = hsa_executable_get_symbol_by_name(executable, kernelname.c_str(), &hsa_dev.agent, &kernel_symbol);
        CHECK_HSA(status, "hsa_executable_get_symbol_by_name()");

        status = hsa_executable_symbol_get_info(kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernel);
        CHECK_HSA(status, "hsa_executable_symbol_get_info()");
        status = hsa_executable_symbol_get_info(kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &kernarg_segment_size);
        CHECK_HSA(status, "hsa_executable_symbol_get_info()");
        status = hsa_executable_symbol_get_info(kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE, &group_segment_size);
        CHECK_HSA(status, "hsa_executable_symbol_get_info()");
        status = hsa_executable_symbol_get_info(kernel_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE, &private_segment_size);
        CHECK_HSA(status, "hsa_executable_symbol_get_info()");

        hsa_dev.lock();
        kernel_cache[executable.handle].emplace(kernelname, std::make_tuple(kernel, kernarg_segment_size, group_segment_size, private_segment_size));
    } else {
        std::tie(kernel, kernarg_segment_size, group_segment_size, private_segment_size) = kernel_it->second;
    }

    hsa_dev.unlock();

    return std::make_tuple(kernel, kernarg_segment_size, group_segment_size, private_segment_size);
}

#ifdef RUNTIME_ENABLE_JIT
static std::string get_ocml_config(int target) {
    std::string config = R"(
        ; Module anydsl ocml config
        define i32 @__oclc_finite_only_opt() alwaysinline { ret i32 0 }
        define i32 @__oclc_unsafe_math_opt() alwaysinline { ret i32 0 }
        define i32 @__oclc_daz_opt() alwaysinline { ret i32 0 }
        define i32 @__oclc_amd_opt() alwaysinline { ret i32 1 }
        define i32 @__oclc_correctly_rounded_sqrt32() alwaysinline { ret i32 1 }
        define i32 @__oclc_ISA_version() alwaysinline { ret i32 )";
    return config + std::to_string(target) + " }";
}

std::string HSAPlatform::emit_gcn(const std::string& program, const std::string& cpu, const std::string &filename, int opt) const {
    LLVMInitializeAMDGPUTarget();
    LLVMInitializeAMDGPUTargetInfo();
    LLVMInitializeAMDGPUTargetMC();
    LLVMInitializeAMDGPUAsmPrinter();

    llvm::LLVMContext llvm_context;
    llvm::SMDiagnostic diagnostic_err;
    std::unique_ptr<llvm::Module> llvm_module = llvm::parseIR(llvm::MemoryBuffer::getMemBuffer(program)->getMemBufferRef(), diagnostic_err, llvm_context);

    auto triple_str = llvm_module->getTargetTriple();
    std::string error_str;
    auto target = llvm::TargetRegistry::lookupTarget(triple_str, error_str);
    llvm::TargetOptions options;
    options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(triple_str, cpu, "" /* attrs */, options, llvm::Reloc::PIC_, llvm::CodeModel::Kernel, llvm::CodeGenOpt::Aggressive));

    // link ocml.amdgcn, irif.amdgcn, and ocml config
    std::string ocml_file = "/opt/rocm/lib/ocml.amdgcn.bc";
    std::string irif_file = "/opt/rocm/lib/irif.amdgcn.bc";
    if (cpu.compare(0, 3, "gfx"))
        error("Expected gfx ISA, got %", cpu);
    std::string ocml_config = get_ocml_config(std::stoi(&cpu[3 /*"gfx"*/]));
    std::unique_ptr<llvm::Module> ocml_module(llvm::parseIRFile(ocml_file, diagnostic_err, llvm_context));
    if (ocml_module == nullptr)
        error("Can't create ocml module for '%'", ocml_file);
    std::unique_ptr<llvm::Module> irif_module(llvm::parseIRFile(irif_file, diagnostic_err, llvm_context));
    if (irif_module == nullptr)
        error("Can't create irif module for '%'", irif_file);
    std::unique_ptr<llvm::Module> config_module = llvm::parseIR(llvm::MemoryBuffer::getMemBuffer(ocml_config)->getMemBufferRef(), diagnostic_err, llvm_context);
    if (config_module == nullptr)
        error("Can't create ocml config module");

    // override data layout with the one coming from the target machine
    llvm_module->setDataLayout(machine->createDataLayout());
    ocml_module->setDataLayout(machine->createDataLayout());
    irif_module->setDataLayout(machine->createDataLayout());
    config_module->setDataLayout(machine->createDataLayout());

    llvm::Linker linker(*llvm_module.get());
    if (linker.linkInModule(std::move(config_module), llvm::Linker::Flags::None))
        error("Can't link config into module");
    if (linker.linkInModule(std::move(ocml_module), llvm::Linker::Flags::LinkOnlyNeeded))
        error("Can't link ocml into module");
    if (linker.linkInModule(std::move(irif_module), llvm::Linker::Flags::LinkOnlyNeeded))
        error("Can't link irif into module");

    llvm::legacy::FunctionPassManager function_pass_manager(llvm_module.get());
    llvm::legacy::PassManager module_pass_manager;

    module_pass_manager.add(llvm::createTargetTransformInfoWrapperPass(machine->getTargetIRAnalysis()));
    function_pass_manager.add(llvm::createTargetTransformInfoWrapperPass(machine->getTargetIRAnalysis()));

    llvm::PassManagerBuilder builder;
    builder.OptLevel = opt;
    builder.Inliner = llvm::createFunctionInliningPass(builder.OptLevel, 0, false);
    machine->adjustPassManager(builder);
    builder.populateFunctionPassManager(function_pass_manager);
    builder.populateModulePassManager(module_pass_manager);

    machine->Options.MCOptions.AsmVerbose = true;

    llvm::SmallString<0> outstr;
    llvm::raw_svector_ostream llvm_stream(outstr);

#if LLVM_VERSION_MAJOR >= 7
    //machine->addPassesToEmitFile(module_pass_manager, llvm_stream, nullptr, llvm::TargetMachine::CGFT_AssemblyFile, true);
    machine->addPassesToEmitFile(module_pass_manager, llvm_stream, nullptr, llvm::TargetMachine::CGFT_ObjectFile, true);
#else
    //machine->addPassesToEmitFile(module_pass_manager, llvm_stream, llvm::TargetMachine::CGFT_AssemblyFile, true);
    machine->addPassesToEmitFile(module_pass_manager, llvm_stream, llvm::TargetMachine::CGFT_ObjectFile, true);
#endif

    function_pass_manager.doInitialization();
    for (auto func = llvm_module->begin(); func != llvm_module->end(); ++func)
        function_pass_manager.run(*func);
    function_pass_manager.doFinalization();
    module_pass_manager.run(*llvm_module);

    std::string obj(outstr.begin(), outstr.end());
    std::string obj_file = filename + ".obj";
    std::string gcn_file = filename + ".gcn";
    store_file(obj_file, obj);
    std::string lld_cmd = "ld.lld -shared " + obj_file + " -o " + gcn_file;
    if (std::system(lld_cmd.c_str()))
        error("Generating gcn using lld");

    return load_file(gcn_file);
}
#else
std::string HSAPlatform::emit_gcn(const std::string&, const std::string&, const std::string &, int) const {
    error("Recompile runtime with RUNTIME_JIT enabled for gcn support.");
}
#endif

std::string HSAPlatform::compile_gcn(DeviceId dev, const std::string& filename, const std::string& program_string) const {
    debug("Compiling AMDGPU to GCN using amdgpu for '%' on HSA device %", filename, dev);
    return emit_gcn(program_string, devices_[dev].isa, filename, 3);
}
