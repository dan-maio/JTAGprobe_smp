/*
 * micro-os-plus-rp2040.h -- what the RP2040 SMP port offers an application,
 * beyond the kernel's own API (cmsis-plus/rtos/os.h, os-c-api.h).
 */

#ifndef MICRO_OS_PLUS_RP2040_H_
#define MICRO_OS_PLUS_RP2040_H_

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Start core 1 under the scheduler.
   *
   * @details
   * Call it once, from a thread on core 0 (os_main() is one), after the
   * threads that must start on core 0 are created. It registers core 1's
   * idle thread, launches core 1 through the Pico SDK's handshake, and then
   * enables core 0's inter-core IPI. From then on a thread whose affinity
   * allows core 1 can run there.
   */
  void
  os_rp2040_start_core1 (void);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_OS_PLUS_RP2040_H_ */
