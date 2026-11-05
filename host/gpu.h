// D3D11 renderer for the recompiled game's Xenos command stream. Called from gpu_exec on the GPU thread.
#pragma once
#include <cstdint>

bool gpu_enabled();                                                     // false when BCR2_NOGPU is set (null GPU behaviour)
bool gpu_tracing();                                                     // true during the frame chosen by BCR2_DUMPFRAME
void gpu_note_wait(int kind, uint64_t qpcTicks);                        // GPU thread blocked: 0 ring empty, 1 WAIT_REG_MEM, 2 interrupt
bool gpu_wait_vblank();                                                 // blocks until the window's display next refreshes; false before the device exists
void gpu_set_display_locked(bool locked);                               // the vsync tick follows the display, so Present can wait for its refresh
void gpu_note_press();                                                 // simulated input changed: logs the frame, and starts a BCR2_SHOTBURST capture
void gpu_reg(uint32_t reg, uint32_t val);                               // type-0 / type-1 register write
void gpu_packet(uint8_t* base, uint32_t op, const uint32_t* b, uint32_t cnt);   // type-3 packet that passed its predicate
