if(is_android OR is_linux OR is_chromeos)
  set(angle_dma_buf_sources
    "src/common/linux/dma_buf_utils.cpp"
    "src/common/linux/dma_buf_utils.h"
  )
endif()
