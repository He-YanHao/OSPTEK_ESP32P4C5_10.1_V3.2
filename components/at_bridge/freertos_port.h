#ifndef FREERTOS_PORT_H
#define FREERTOS_PORT_H

#if defined(ESP_PLATFORM)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/semphr.h"
  #include "freertos/stream_buffer.h"
#else
  #include "FreeRTOS.h"
  #include "task.h"
  #include "semphr.h"
  #include "stream_buffer.h"
#endif

#endif /* FREERTOS_PORT_H */