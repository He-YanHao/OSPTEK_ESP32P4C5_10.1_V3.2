#ifndef FREERTOS_PORT_H
#define FREERTOS_PORT_H

/*
 * ============================================================
 *  FreeRTOS 头文件适配
 * ============================================================
 *
 * 不同平台的 FreeRTOS 头文件路径不同：
 *   - ESP-IDF：freertos/FreeRTOS.h 等（带 freertos/ 前缀）
 *   - 裸 FreeRTOS：FreeRTOS.h 等（无前缀）
 *
 * 本文件统一入口，其他跨平台文件只 include 本文件。
 */

#if defined(ESP_PLATFORM)
  /* ESP-IDF 平台：头文件都在 freertos/ 目录下 */
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/semphr.h"
  #include "freertos/stream_buffer.h"
#else
  /* 裸 FreeRTOS（STM32 等）：无前缀 */
  #include "FreeRTOS.h"
  #include "task.h"
  #include "semphr.h"
  #include "stream_buffer.h"
#endif

#endif /* FREERTOS_PORT_H */