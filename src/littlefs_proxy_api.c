/**
 * @file littlefs_api.c
 * @brief Maps the HAL of esp_partition <-> littlefs via a proxy task.
 * @author Brian Pugh
 * @author alufers
 * 
 * All flash operations are performed by a proxy task with the stack in IRAM.
 * This is so that littlefs operations can be performed from tasks with
 * the stack in PSRAM.
 * 
 */

//#define ESP_LOCAL_LOG_LEVEL ESP_LOG_INFO

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "littlefs/lfs.h"
#include "littlefs_api.h"

/**
 * @brief The type of operation that is to be performed by the flash proxy task.
 * 
 */
typedef enum flash_op_type_t {
  FLASH_OP_NONE = 0,
  FLASH_OP_READ = 1,
  FLASH_OP_WRITE = 2,
  FLASH_OP_ERASE = 3,
  FLASH_OP_INVALID = 4,
} flash_op_type_t;

/**
 * @brief The operation that is to be performed by the flash proxy task.
 * 
 */
typedef struct flash_op_t {
  flash_op_type_t type;
  const esp_partition_t* partition;
  size_t part_off;
  void* buffer;
  size_t size; /* in bytes */
} flash_op_t;

/**
 * @brief Samaphore that is given when the flash proxy is done with the operation
 */
static SemaphoreHandle_t operation_done_sem = NULL;

/**
 * @brief Handle to the flash proxy task
 * 
 */
static TaskHandle_t flash_proxy_task_handle = NULL;

/**
 * @brief The operation that is supposed to be performed by the flash proxy task.
 * 
 */
static flash_op_t current_flash_op = {0};

/**
 * @brief The result of the flash operation. Shall be read after the semaphore is given.
 * 
 */
static int flash_op_result = 0;

/**
 * @brief A buffer stored in IRAM that contains the data to be written or read.
 */
static uint8_t* flash_proxy_buf = NULL;

/**
 * @brief The size of the flash_proxy_buf, so it can be reallocated if needed.
 */
static size_t flash_proxy_buf_size = 0;

static void flash_proxy_task(void* pvParameters);

void start_flash_proxy_task() {

  if (flash_proxy_task_handle == NULL) {
    operation_done_sem = xSemaphoreCreateBinary();
    xTaskCreate(flash_proxy_task, "flash_proxy_task", 4096, NULL, 5,
                &flash_proxy_task_handle);
  }
}

static void flash_proxy_task(void* pvParameters) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_err_t err;
    flash_op_result = 0;
    const char* op_str = "unknown";
    switch (current_flash_op.type) {
      case FLASH_OP_READ:

        op_str = "read";
        err = esp_partition_read(
            current_flash_op.partition, current_flash_op.part_off,
            current_flash_op.buffer, current_flash_op.size);
        break;
      case FLASH_OP_WRITE:
        op_str = "write";
        err = esp_partition_write(
            current_flash_op.partition, current_flash_op.part_off,
            current_flash_op.buffer, current_flash_op.size);
        break;
      case FLASH_OP_ERASE:
        op_str = "erase";
        err = esp_partition_erase_range(current_flash_op.partition,
                                        current_flash_op.part_off,
                                        current_flash_op.size);
        break;
      default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    if (err) {
      ESP_LOGE(ESP_LITTLEFS_TAG, "failed to perform flash op, err %d", err);
      flash_op_result = LFS_ERR_IO;
    }
    xSemaphoreGive(operation_done_sem);
  }
}

static void ensure_flash_proxy_buf_size(size_t size) {
  if (flash_proxy_buf == NULL) {
    flash_proxy_buf =
        heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    flash_proxy_buf_size = size;
  } else if (flash_proxy_buf_size < size) {
    flash_proxy_buf = heap_caps_realloc(flash_proxy_buf, size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    flash_proxy_buf_size = size;
  }
}

int littlefs_api_read(const struct lfs_config* c, lfs_block_t block,
                      lfs_off_t off, void* buffer, lfs_size_t size) {
  esp_littlefs_t* efs = c->context;
  size_t part_off = (block * c->block_size) + off;
  if (flash_proxy_task_handle == NULL) {
    ESP_LOGE(ESP_LITTLEFS_TAG, "flash proxy task not started");
    return LFS_ERR_IO;
  }
  ensure_flash_proxy_buf_size(size);

  current_flash_op.type = FLASH_OP_READ;
  current_flash_op.partition = efs->partition;
  current_flash_op.part_off = part_off;
  current_flash_op.buffer = flash_proxy_buf;
  current_flash_op.size = size;
  xTaskNotifyGive(flash_proxy_task_handle);
  xSemaphoreTake(operation_done_sem, portMAX_DELAY);

  memcpy(buffer, flash_proxy_buf, size);

  return 0;
}

int littlefs_api_prog(const struct lfs_config* c, lfs_block_t block,
                      lfs_off_t off, const void* buffer, lfs_size_t size) {
  esp_littlefs_t* efs = c->context;
  size_t part_off = (block * c->block_size) + off;
  if (flash_proxy_task_handle == NULL) {
    ESP_LOGE(ESP_LITTLEFS_TAG, "flash proxy task not started");
    return LFS_ERR_IO;
  }
  ensure_flash_proxy_buf_size(size);
  memcpy(flash_proxy_buf, buffer, size);
  current_flash_op.type = FLASH_OP_WRITE;
  current_flash_op.partition = efs->partition;
  current_flash_op.part_off = part_off;
  current_flash_op.buffer = flash_proxy_buf;
  current_flash_op.size = size;

  xTaskNotifyGive(flash_proxy_task_handle);
  xSemaphoreTake(operation_done_sem, portMAX_DELAY);

  return 0;
}

int littlefs_api_erase(const struct lfs_config* c, lfs_block_t block) {
  esp_littlefs_t* efs = c->context;
  size_t part_off = block * c->block_size;
  if (flash_proxy_task_handle == NULL) {
    ESP_LOGE(ESP_LITTLEFS_TAG, "flash proxy task not started");
    return LFS_ERR_IO;
  }
  current_flash_op.type = FLASH_OP_ERASE;
  current_flash_op.partition = efs->partition;
  current_flash_op.part_off = part_off;
  current_flash_op.size = c->block_size;
  current_flash_op.buffer = NULL;
  xTaskNotifyGive(flash_proxy_task_handle);
  xSemaphoreTake(operation_done_sem, portMAX_DELAY);

  return 0;
}

int littlefs_api_sync(const struct lfs_config* c) {
  /* Unnecessary for esp-idf */
  return 0;
}
