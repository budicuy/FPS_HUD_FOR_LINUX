# CMake generated Testfile for 
# Source directory: /home/budicuy/Dokumen/FPS_HUD_FOR_LINUX
# Build directory: /home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/build_tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("fpshud_test" "/home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/build_tsan/fpshud_test")
set_tests_properties("fpshud_test" PROPERTIES  ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7" TIMEOUT "120" WORKING_DIRECTORY "/home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/build_tsan" _CMAKE_TEST_BUILD_DEPENDS "/home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/build_tsan/fpshud_test" _BACKTRACE_TRIPLES "/home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/CMakeLists.txt;432;add_test;/home/budicuy/Dokumen/FPS_HUD_FOR_LINUX/CMakeLists.txt;0;")
