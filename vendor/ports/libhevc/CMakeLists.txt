cmake_minimum_required(VERSION 3.14)
project(libhevc C)

set(HEVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Threads REQUIRED)

# --- Platform detection ---
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86|X86")
    set(HEVC_X86 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(HEVC_AARCH64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|ARM")
    set(HEVC_AARCH32 TRUE)
endif()

# --- Common compile definitions ---
set(HEVC_DEFS "")
if(HEVC_X86)
    list(APPEND HEVC_DEFS X86 DISABLE_AVX2 DEFAULT_ARCH=D_ARCH_X86_SSE42)
    if(NOT WIN32)
        list(APPEND HEVC_DEFS X86_LINUX=1)
    endif()
elseif(HEVC_AARCH64)
    list(APPEND HEVC_DEFS ARMV8 DEFAULT_ARCH=D_ARCH_ARMV8_GENERIC ENABLE_NEON)
elseif(HEVC_AARCH32)
    list(APPEND HEVC_DEFS ARMV7 DEFAULT_ARCH=D_ARCH_ARM_A9Q ENABLE_NEON DISABLE_NEONINTR)
endif()

if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    list(APPEND HEVC_DEFS MSVC)
    # clang-cl does not support bare asm() — use __asm__ instead
    list(APPEND HEVC_DEFS "asm=__asm__")
endif()

# --- Common compile options ---
set(HEVC_COPTS "")
if(HEVC_X86 AND NOT MSVC)
    list(APPEND HEVC_COPTS -msse4.2 -mno-avx)
endif()

# --- Common sources ---
if(WIN32)
    set(ITHREAD_SRC common/ithread_win32.c)
else()
    set(ITHREAD_SRC common/ithread.c)
endif()

set(COMMON_SRCS
    ${ITHREAD_SRC}
    common/ihevc_quant_tables.c
    common/ihevc_inter_pred_filters.c
    common/ihevc_weighted_pred.c
    common/ihevc_padding.c
    common/ihevc_deblk_edge_filter.c
    common/ihevc_deblk_tables.c
    common/ihevc_cabac_tables.c
    common/ihevc_common_tables.c
    common/ihevc_intra_pred_filters.c
    common/ihevc_chroma_intra_pred_filters.c
    common/ihevc_mem_fns.c
    common/ihevc_sao.c
    common/ihevc_trans_tables.c
    common/ihevc_recon.c
    common/ihevc_itrans.c
    common/ihevc_itrans_recon.c
    common/ihevc_iquant_recon.c
    common/ihevc_iquant_itrans_recon.c
    common/ihevc_itrans_recon_32x32.c
    common/ihevc_itrans_recon_16x16.c
    common/ihevc_itrans_recon_8x8.c
    common/ihevc_chroma_itrans_recon.c
    common/ihevc_chroma_iquant_recon.c
    common/ihevc_chroma_iquant_itrans_recon.c
    common/ihevc_chroma_recon.c
    common/ihevc_chroma_itrans_recon_16x16.c
    common/ihevc_chroma_itrans_recon_8x8.c
    common/ihevc_buf_mgr.c
    common/ihevc_disp_mgr.c
    common/ihevc_dpb_mgr.c
    common/ihevc_hbd_deblk_edge_filter.c
    common/ihevc_quant_iquant_ssd.c
    common/ihevc_resi_trans.c
)

if(HEVC_X86)
    list(APPEND COMMON_SRCS
        common/x86/ihevc_inter_pred_filters_ssse3_intr.c
        common/x86/ihevc_weighted_pred_ssse3_intr.c
        common/x86/ihevc_intra_pred_filters_ssse3_intr.c
        common/x86/ihevc_chroma_intra_pred_filters_ssse3_intr.c
        common/x86/ihevc_itrans_recon_ssse3_intr.c
        common/x86/ihevc_itrans_recon_16x16_ssse3_intr.c
        common/x86/ihevc_itrans_recon_32x32_ssse3_intr.c
        common/x86/ihevc_sao_ssse3_intr.c
        common/x86/ihevc_deblk_ssse3_intr.c
        common/x86/ihevc_padding_ssse3_intr.c
        common/x86/ihevc_mem_fns_ssse3_intr.c
        common/x86/ihevc_inter_pred_filters_sse42_intr.c
        common/x86/ihevc_weighted_pred_sse42_intr.c
        common/x86/ihevc_intra_pred_filters_sse42_intr.c
        common/x86/ihevc_chroma_intra_pred_filters_sse42_intr.c
        common/x86/ihevc_itrans_recon_sse42_intr.c
        common/x86/ihevc_16x16_itrans_recon_sse42_intr.c
        common/x86/ihevc_32x32_itrans_recon_sse42_intr.c
        common/x86/ihevc_tables_x86_intr.c
    )
    set(COMMON_X86_INCDIR common/x86)
endif()

# Prepend HEVC_ROOT
list(TRANSFORM COMMON_SRCS PREPEND "${HEVC_ROOT}/")

# --- Decoder sources ---
set(DEC_SRCS
    decoder/ihevcd_version.c
    decoder/ihevcd_api.c
    decoder/ihevcd_decode.c
    decoder/ihevcd_nal.c
    decoder/ihevcd_bitstream.c
    decoder/ihevcd_parse_headers.c
    decoder/ihevcd_parse_slice_header.c
    decoder/ihevcd_parse_slice.c
    decoder/ihevcd_parse_residual.c
    decoder/ihevcd_cabac.c
    decoder/ihevcd_intra_pred_mode_prediction.c
    decoder/ihevcd_process_slice.c
    decoder/ihevcd_utils.c
    decoder/ihevcd_job_queue.c
    decoder/ihevcd_ref_list.c
    decoder/ihevcd_get_mv.c
    decoder/ihevcd_mv_pred.c
    decoder/ihevcd_mv_merge.c
    decoder/ihevcd_iquant_itrans_recon_ctb.c
    decoder/ihevcd_itrans_recon_dc.c
    decoder/ihevcd_common_tables.c
    decoder/ihevcd_boundary_strength.c
    decoder/ihevcd_deblk.c
    decoder/ihevcd_inter_pred.c
    decoder/ihevcd_sao.c
    decoder/ihevcd_ilf_padding.c
    decoder/ihevcd_fmt_conv.c
)

if(HEVC_X86)
    list(APPEND DEC_SRCS
        decoder/x86/ihevcd_function_selector.c
        decoder/x86/ihevcd_function_selector_generic.c
        decoder/x86/ihevcd_function_selector_ssse3.c
        decoder/x86/ihevcd_function_selector_sse42.c
        decoder/x86/ihevcd_fmt_conv_ssse3_intr.c
        decoder/x86/ihevcd_it_rec_dc_ssse3_intr.c
        decoder/x86/ihevcd_it_rec_dc_sse42_intr.c
    )
    set(DEC_X86_INCDIR decoder/x86)
endif()

list(TRANSFORM DEC_SRCS PREPEND "${HEVC_ROOT}/")

# --- Encoder sources (POSIX only — encoder OSAL uses pthreads/semaphore.h) ---
if(NOT WIN32)
set(ENC_SRCS
    encoder/bit_allocation.c
    encoder/cbr_buffer_control.c
    encoder/common_rom.c
    encoder/convert_float_to_fix.c
    encoder/est_sad.c
    encoder/fixed_point_error_bits.c
    encoder/frame_info_collector.c
    encoder/hme_coarse.c
    encoder/hme_common_utils.c
    encoder/hme_err_compute.c
    encoder/hme_fullpel.c
    encoder/hme_function_selector.c
    encoder/hme_globals.c
    encoder/hme_interface.c
    encoder/hme_refine.c
    encoder/hme_search_algo.c
    encoder/hme_subpel.c
    encoder/hme_utils.c
    encoder/ihevce_bitstream.c
    encoder/ihevce_bs_compute_ctb.c
    encoder/ihevce_buffer_que.c
    encoder/ihevce_cabac.c
    encoder/ihevce_cabac_cu_pu.c
    encoder/ihevce_cabac_rdo.c
    encoder/ihevce_cabac_tu.c
    encoder/ihevce_chroma_had_satd.c
    encoder/ihevce_cmn_utils_instr_set_router.c
    encoder/ihevce_coarse_me_pass.c
    encoder/ihevce_common_utils.c
    encoder/ihevce_deblk.c
    encoder/ihevce_decomp_pre_intra_pass.c
    encoder/ihevce_dep_mngr.c
    encoder/ihevce_enc_cu_recursion.c
    encoder/ihevce_enc_loop_inter_mode_sifter.c
    encoder/ihevce_enc_loop_pass.c
    encoder/ihevce_enc_loop_utils.c
    encoder/ihevce_enc_sbh_funcs.c
    encoder/ihevce_enc_subpel_gen.c
    encoder/ihevce_encode_header.c
    encoder/ihevce_encode_header_sei_vui.c
    encoder/ihevce_entropy_cod.c
    encoder/ihevce_entropy_interface.c
    encoder/ihevce_error_check.c
    encoder/ihevce_frame_process.c
    encoder/ihevce_frame_process_utils.c
    encoder/ihevce_function_selector.c
    encoder/ihevce_global_tables.c
    encoder/ihevce_had_satd.c
    encoder/ihevce_hle_interface.c
    encoder/ihevce_hle_q_func.c
    encoder/ihevce_inter_pred.c
    encoder/ihevce_ipe_instr_set_router.c
    encoder/ihevce_ipe_pass.c
    encoder/ihevce_lap_interface.c
    encoder/ihevce_me_instr_set_router.c
    encoder/ihevce_me_pass.c
    encoder/ihevce_memory_init.c
    encoder/ihevce_multi_thrd_funcs.c
    encoder/ihevce_mv_pred.c
    encoder/ihevce_mv_pred_merge.c
    encoder/ihevce_nbr_avail.c
    encoder/ihevce_plugin.c
    encoder/ihevce_profile.c
    encoder/ihevce_rc_interface.c
    encoder/ihevce_recur_bracketing.c
    encoder/ihevce_sao.c
    encoder/ihevce_stasino_helpers.c
    encoder/ihevce_sub_pic_rc.c
    encoder/ihevce_sys_api.c
    encoder/ihevce_tile_interface.c
    encoder/ihevce_trace.c
    encoder/ihevce_tu_tree_selector.c
    encoder/init_qp.c
    encoder/mb_model_based.c
    encoder/osal.c
    encoder/osal_cond_var.c
    encoder/osal_error.c
    encoder/osal_mutex.c
    encoder/osal_semaphore.c
    encoder/osal_thread.c
    encoder/picture_type.c
    encoder/rate_control_api.c
    encoder/rc_rd_model.c
    encoder/rc_rd_model_fix.c
    encoder/rc_sad_acc.c
    encoder/sqrt_interp.c
    encoder/var_q_operator.c
    encoder/vbr_storage_vbv.c
    encoder/vbr_str_prms.c
)

list(TRANSFORM ENC_SRCS PREPEND "${HEVC_ROOT}/")
endif() # NOT WIN32

# --- Targets ---

# Common library (shared between decoder and encoder)
add_library(hevc_common STATIC ${COMMON_SRCS})
target_include_directories(hevc_common
    PUBLIC
        $<BUILD_INTERFACE:${HEVC_ROOT}/common>
        $<$<BOOL:${HEVC_X86}>:$<BUILD_INTERFACE:${HEVC_ROOT}/common/x86>>
        $<INSTALL_INTERFACE:include/libhevc>
)
target_compile_definitions(hevc_common PRIVATE ${HEVC_DEFS})
target_compile_options(hevc_common PRIVATE ${HEVC_COPTS})
target_link_libraries(hevc_common PUBLIC Threads::Threads)
if(NOT WIN32)
    target_link_libraries(hevc_common PUBLIC m)
endif()

# Decoder
add_library(hevcdec STATIC ${DEC_SRCS})
target_include_directories(hevcdec
    PUBLIC
        $<BUILD_INTERFACE:${HEVC_ROOT}/decoder>
        $<BUILD_INTERFACE:${HEVC_ROOT}/common>
        $<INSTALL_INTERFACE:include/libhevc>
    PRIVATE
        ${HEVC_ROOT}/decoder
        $<$<BOOL:${HEVC_X86}>:${HEVC_ROOT}/${DEC_X86_INCDIR}>
)
target_compile_definitions(hevcdec PRIVATE ${HEVC_DEFS})
target_compile_options(hevcdec PRIVATE ${HEVC_COPTS})
target_link_libraries(hevcdec PUBLIC hevc_common)

# Encoder (POSIX only)
set(INSTALL_TARGETS hevc_common hevcdec)
if(NOT WIN32)
    add_library(hevcenc STATIC ${ENC_SRCS})
    target_include_directories(hevcenc
        PUBLIC
            $<BUILD_INTERFACE:${HEVC_ROOT}/encoder>
            $<BUILD_INTERFACE:${HEVC_ROOT}/common>
            $<INSTALL_INTERFACE:include/libhevc>
        PRIVATE
            ${HEVC_ROOT}/encoder
    )
    target_compile_definitions(hevcenc PRIVATE ${HEVC_DEFS} N_MB_ENABLE ENABLE_MAIN_REXT_PROFILE)
    target_compile_options(hevcenc PRIVATE ${HEVC_COPTS})
    target_link_libraries(hevcenc PUBLIC hevc_common)
    list(APPEND INSTALL_TARGETS hevcenc)
endif()

# --- Install ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ${INSTALL_TARGETS}
    EXPORT libhevc-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install public headers
set(PUBLIC_HEADERS
    common/iv.h
    common/ivd.h
    common/ihevc_typedefs.h
    common/ihevc_defs.h
    common/ihevc_structs.h
    common/ihevc_macros.h
    common/ihevc_debug.h
    common/ithread.h
    decoder/ihevcd_cxa.h
)
foreach(hdr ${PUBLIC_HEADERS})
    if(EXISTS "${HEVC_ROOT}/${hdr}")
        install(FILES "${HEVC_ROOT}/${hdr}" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libhevc)
    endif()
endforeach()

install(EXPORT libhevc-targets
    FILE libhevc-targets.cmake
    NAMESPACE libhevc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libhevc
)

configure_package_config_file(
    "${HEVC_ROOT}/cmake/libhevc-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libhevc-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libhevc
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libhevc-config.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libhevc
)
