add_executable(ftr_fake_riscv_sim
  fake_riscv_sim.cpp
)

target_compile_features(ftr_fake_riscv_sim PRIVATE cxx_std_17)
target_link_libraries(ftr_fake_riscv_sim PRIVATE ftr_trace)
