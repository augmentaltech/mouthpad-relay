# SPDX-License-Identifier: Apache-2.0
# Bring-up: flashed flat over J-Link (no product bootloader yet).

board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")
board_runner_args(nrfjprog "--nrf-family=NRF52")

include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
