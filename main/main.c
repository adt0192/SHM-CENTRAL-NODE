///////////////////////////////////////////////////////////////////////////////
//************************************D´A************************************//
///////////////////////////////////////////////////////////////////////////////
// *****************************************************************************
// ** Project name:               Reyax LoRa Module RYLR998 Testing
// ** Created by:                 Andy Duarte Taño
// ** Created:                    06/11/2023
// ** Version:                    RX Code
// ** Last modified:
// ** Software:                   C/C++, ESP-IDF Framework, PlatformIO
// ** Hardware:                   ESP32-S3-DevKit-C1, Reyax RYLR998 LoRa Module
// ** Contact:                    andyduarte0192@gmail.com

// ** Copyright (c) 2023, Andy Duarte Taño. All rights reserved.

// ** This code is free to use for any purpose as long as the author is cited.

// ** Code description:
// *****************************************************************************
///////////////////////////////////////////////////////////////////////////////
//************************************D´A************************************//
///////////////////////////////////////////////////////////////////////////////

// Users can call heap_caps_get_free_size(MALLOC_CAP_8BIT) to get the free size
// of all DRAM heaps.

///////////////////////////////////////////////////////////////////////////////
//************************************+ +************************************//
///////////////////////////////////////////////////////////////////////////////

//***************************************************************************//
//************************************+ +************************************//
//***************************************************************************//
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "numerical_systems_conv.h"
#include "rylr998.h"
#include "sdkconfig.h"
#include <FreeRTOSConfig.h>
#include <freertos/ringbuf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//***************************************************************************//
//************************************+ +************************************//
//***************************************************************************//

#define LED_PIN GPIO_NUM_2

// spi accelerometer interface
/**
 * @brief Sample rates in Hertz (Hz)
 *
 */
typedef enum {
  F_31_5HZ = 0x00, /**< Speed at 31.5 samples per second*/
  F_125HZ          /**< Speed at 125 samples per second */
} frequency_t;

/**
 * @brief Scale configuration in g
 *
 */
typedef enum {
  SCALE_2G = 0x00, /**< ±2g Scale */
  SCALE_4G,        /**< ±4g Scale */
  SCALE_8G,        /**< ±8g Scale */
} scale_t;

//***************************************************************************//
//********************************** FLAGS **********************************//
//***************************************************************************//
char *is_data_sent_ok = "0";    // we dont need this here
char *is_duplicated_data = "N"; // to kmow if the message is a retransmission
char *is_sending_ack = "N";
char *is_rylr998_module_init = "N";

// to keep track when we already have received a full block of data from the
// sensonr node, because it will be split into 120 bytes sub-block
// the full block received from the sensor node it's max 256 bytes
bool start_uart_block = false;
bool end_uart_block = false; // not used

// to know when we have a 'ctrl' so we extract the info needed
//     x_bits_tx       y_bits_tx       z_bits_tx
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|     <<PAYLOAD...>>
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// |    32 bits    |    32 bits    |    32 bits    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |  min_x_value  |  min_y_value  |  min_z_value  |     <<...PAYLOAD>>
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
bool is_ctrl_msg = false;

// to know the type of message we received
msg_type in_msg_type;
//***************************************************************************//
//********************************** FLAGS **********************************//
//***************************************************************************//

//***************************************************************************//
//***************************** HANDLE VARIABLES ****************************//
//***************************************************************************//
static QueueHandle_t uart_queue;

// Handles for the tasks
static TaskHandle_t check_header_incoming_data_task_handle = NULL,
                    transmit_ack_task_handle = NULL,
                    extract_info_from_ctrl_msg_task_handle = NULL;
//***************************************************************************//
//***************************** HANDLE VARIABLES ****************************//
//***************************************************************************//

//***************************************************************************//
//***************************** GLOBAL VARIABLES ****************************//
//***************************************************************************//
// testing parameters
frequency_t test_freq = F_125HZ;
scale_t test_scale = SCALE_8G;

// to store the 'ctrl' message received to work with it
char *in_ctrl_msg = NULL;

// RingBuffer variables
#define RINGBUFFER_SIZE 4096
#define RINGBUFFER_TYPE RINGBUF_TYPE_BYTEBUF

#define INCOMING_UART_DATA_SIZE 128
#define FULL_IN_UART_DATA_SIZE 260

// max and min values of each axis set of samples
double min_x_value = 0; // min x initializing
double max_x_value = 0; // max x initializing
double min_y_value = 0; // min y initializing
double max_y_value = 0; // max y initializing
double min_z_value = 0; // min z initializing
double max_z_value = 0; // max z initializing

// to keep tracking of the sample we are extracting from
// *_samples_compressed_bin that we are copying to total_bits_tx_after_pad0
int d_a = 0;

// needed bits for each axis to transmit their respective samples
uint8_t x_bits, y_bits, z_bits;

Lora_Data_t Lora_data;

// the value below will not be initialized at startup and should keep its value
// after software restart
uint16_t MSG_COUNTER_RX = 0; // counter to set the message transaction ID

#define CR 10
#define N 1024     // number of samples
#define p (N / CR) // number of compressed samples

// all next will store all samples taken (N rows and 3 columns)
// 3 columns for each acceleration data
// XDATA3-XDATA2-XDATA1
// YDATA3-YDATA2-YDATA1
// ZDATA3-ZDATA2-ZDATA1
uint8_t **x_samples; // x measurements
uint8_t **y_samples; // y measurements
uint8_t **z_samples; // z measurements

// all next will have all samples re-arranged,
// and decoded (double type)
double *x_samples_double; // x measurements
double *y_samples_double; // y measurements
double *z_samples_double; // z measurements

// all next will have compressed measurement vector
double *x_samples_compressed; // x measurements
double *y_samples_compressed; // y measurements
double *z_samples_compressed; // z measurements

// array of pointers for the binary representation of the position of a
// sample in the new interval [0 ~ (max + |min|)]
char *x_samples_compressed_bin[p];
char *y_samples_compressed_bin[p];
char *z_samples_compressed_bin[p];

int64_t temp_time0, temp_time1;

//***************************************************************************//
//***************************** GLOBAL VARIABLES ****************************//
//***************************************************************************//

static const char *TAG = "SHM CENTRAL NODE";

//////////////////////////////////////////////////////////////////////////
/////////////// LoRa Data Message Format (designed by me) //////////////////
//////////////////////////////////////////////////////////////////////////
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |     byte 1    |    byte 2     |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7|
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// | T |       Transaction ID      |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

// T -> message type (reset, control, data, ack)
// Transaction ID -> counter of the message to send
//////////////////////////////////////////////////////////////////////////
/////////////// LoRa Data Message Format (designed by me) //////////////////
//////////////////////////////////////////////////////////////////////////

//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+//
//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+//
//* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *//
//* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *//
//***************************** FUNCTIONS SECTION ***************************//
//********************************** BELOW **********************************//
//* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *//
//* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *//
//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+//
//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+//

///////////////////////////////////////////////////////////////////////////////
//************************** Initialization of LED  *************************//
///////////////////////////////////////////////////////////////////////////////
void init_led(void) {
  ESP_ERROR_CHECK(gpio_reset_pin(LED_PIN));
  ESP_ERROR_CHECK(gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT));

  gpio_set_level(LED_PIN, 0);

  ESP_LOGI(TAG, "Init LED !!!COMPLETED!!!");
}
///////////////////////////////////////////////////////////////////////////////
//************************** Initialization of LED  *************************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//*************** Extract info from the received ctrl message ***************//
///////////////////////////////////////////////////////////////////////////////
static void extract_info_from_ctrl_msg_task(void *pvParameters) {
  // to temporarily store the hexadecimal charactes for xyz amount of bits
  char *x_bits_hex = NULL;
  char *y_bits_hex = NULL;
  char *z_bits_hex = NULL;

  // to temporarily store the hexadecimal charactes for xyz min values
  char *min_x_value_hex = NULL;
  char *min_y_value_hex = NULL;
  char *min_z_value_hex = NULL;

  // to store the 4 uint8_t values that each xyz min values will have
  // to combine them to form a unique int32_t value
  uint8_t *min_x_value_8bit_arr = NULL;
  uint8_t *min_y_value_8bit_arr = NULL;
  uint8_t *min_z_value_8bit_arr = NULL;
  while (1) {
    // Block to wait for data received (+RCV=) and check if ACK
    // Block indefinitely (without a timeout, so no need to check the function's
    // return value) to wait for a notification. Here the RTOS task notification
    // is being used as a binary semaphore, so the notification value is cleared
    // to zero on exit. NOTE! Real applications should not block indefinitely,
    // but instead time out occasionally in order to handle error conditions
    // that may prevent the interrupt from sending any more notifications.
    ulTaskNotifyTake(pdTRUE,         // Clear the notification value on exit
                     portMAX_DELAY); // Block indefinitely

    // Print out remaining task stack memory (words) ************************
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    ESP_LOGW(TAG, "'extract_info_from_ctrl_msg_task': <%zu>",
             uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    // Print out remaining task stack memory (words) ************************

    //     x_bits_tx       y_bits_tx       z_bits_tx
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|     <<PAYLOAD...>>
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    //
    // |    32 bits    |    32 bits    |    32 bits    |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |  min_x_value  |  min_y_value  |  min_z_value  |     <<...PAYLOAD>>
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // working with the xyz amount of bits values
    x_bits_hex = malloc((2 + 1) * sizeof(*x_bits_hex));
    if (x_bits_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *x_bits_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 0, 2, x_bits_hex);
    x_bits = HexadecimalToDecimal(x_bits_hex);
    free(x_bits_hex);
    //
    y_bits_hex = malloc((2 + 1) * sizeof(*y_bits_hex));
    if (y_bits_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *y_bits_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 2, 2, y_bits_hex);
    y_bits = HexadecimalToDecimal(y_bits_hex);
    free(y_bits_hex);
    //
    z_bits_hex = malloc((2 + 1) * sizeof(*z_bits_hex));
    if (z_bits_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *z_bits_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 4, 2, z_bits_hex);
    z_bits = HexadecimalToDecimal(z_bits_hex);
    free(z_bits_hex);

    // working with xyz min values
    min_x_value_hex = malloc((8 + 1) * sizeof(*min_x_value_hex));
    if (min_x_value_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_x_value_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 6, 8, min_x_value_hex);
    //
    min_y_value_hex = malloc((8 + 1) * sizeof(*min_y_value_hex));
    if (min_y_value_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_y_value_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 14, 8, min_y_value_hex);
    //
    min_z_value_hex = malloc((8 + 1) * sizeof(*min_z_value_hex));
    if (min_z_value_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_z_value_hex in task "
                    "extract_info_from_ctrl_msg_task");
    }
    GetSubString(in_ctrl_msg, 22, 8, min_z_value_hex);

    //                     min_*xyz*_value_8bit_arr
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|0 1 2 3 4 5 6 7|
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    min_x_value_8bit_arr = malloc((4) * sizeof(*min_x_value_8bit_arr));
    if (min_x_value_8bit_arr == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_x_value_8bit_arr in task "
                    "extract_info_from_ctrl_msg_task");
    }
    min_y_value_8bit_arr = malloc((4) * sizeof(*min_y_value_8bit_arr));
    if (min_y_value_8bit_arr == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_y_value_8bit_arr in task "
                    "extract_info_from_ctrl_msg_task");
    }
    min_z_value_8bit_arr = malloc((4) * sizeof(*min_z_value_8bit_arr));
    if (min_z_value_8bit_arr == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *min_z_value_8bit_arr in task "
                    "extract_info_from_ctrl_msg_task");
    }

    // to store temporarily the subdivisions of min_*xyz*_value_hex,
    // into 8-bit words so we can use HexadecimalToDecimal function to
    // get the decimal
    uint8_t tmp_8bits_section_dec;
    char *tmp_8bits_section_hex =
        malloc((2 + 1) * sizeof(*tmp_8bits_section_hex));
    if (tmp_8bits_section_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *tmp_8bits_section_hex in "
                    "extract_info_from_ctrl_msg_task");
    }
    uint8_t dayi = 3;
    // the next 'for' loop will break 'min_*xyz*_value_hex' down into in32_t
    for (size_t i = 0; i < 8; i += 2) {
      // x_min_value
      GetSubString(min_x_value_hex, i, 2, tmp_8bits_section_hex);
      tmp_8bits_section_dec = HexadecimalToDecimal(tmp_8bits_section_hex);
      min_x_value_8bit_arr[i / 2] = tmp_8bits_section_dec;
      // y_min_value
      GetSubString(min_y_value_hex, i, 2, tmp_8bits_section_hex);
      tmp_8bits_section_dec = HexadecimalToDecimal(tmp_8bits_section_hex);
      min_y_value_8bit_arr[i / 2] = tmp_8bits_section_dec;
    }

    // freeing up allocate dspace
    free(in_ctrl_msg);
  }
}
///////////////////////////////////////////////////////////////////////////////
//*************** Extract info from the received ctrl message ***************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//******************** Transmit ACK of the received data ********************//
///////////////////////////////////////////////////////////////////////////////
static void transmit_ack_task(void *pvParameters) {
  ///// header for the data to send
  char *header_hex_of_data_to_send = NULL;
  //
  ///// data to send
  char *data_to_send_hex = NULL;
  //
  ///// full message to send (header + data)
  char *full_message_hex = NULL;
  while (1) {
    // Block to wait for permission to transmit
    // Block indefinitely (without a timeout, so no need to check the function's
    // return value) to wait for a notification. Here the RTOS task notification
    // is being used as a binary semaphore, so the notification value is cleared
    // to zero on exit. NOTE! Real applications should not block indefinitely,
    // but instead time out occasionally in order to handle error conditions
    // that may prevent the interrupt from sending any more notifications.
    ulTaskNotifyTake(pdTRUE,         // Clear the notification value on exit
                     portMAX_DELAY); // Block indefinitely

    // Print out remaining task stack memory (words) ************************
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    ESP_LOGW(TAG, "'transmit_ack_task': <%zu>",
             uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    // Print out remaining task stack memory (words) ************************

    if ((strncmp(is_duplicated_data, "Y", 1) == 0)) {
      // if what we received was a data with a previous transaction ID, we still
      // need to send ACK for that transaction ID, because it means the other
      // side didn't receive our previous 'ack' message
      // so we set the transaction ID of the current 'ack' message to that
      // previous transaction ID
      header_hex_of_data_to_send = add_header_hex(ack, MSG_COUNTER_RX - 1);
    } else {
      header_hex_of_data_to_send = add_header_hex(ack, MSG_COUNTER_RX);
    }

    // full message to send:
    // full_message_hex = data_to_send_header_hex + data_to_send_hex
    // in this case since we are sending ACK, we just send "00\0" as the data
    full_message_hex = malloc((strlen(header_hex_of_data_to_send) + 2 + 1) *
                              sizeof(*full_message_hex));
    if (full_message_hex == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *full_message_hex in task "
                    "transmit_ack_task");
    }

    AppendString(header_hex_of_data_to_send, "00\0", full_message_hex);
    free(header_hex_of_data_to_send);

    gpio_set_level(LED_PIN, 1);
    lora_send(LORA_RX_ADDRESS, full_message_hex); // sending
    free(full_message_hex);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
///////////////////////////////////////////////////////////////////////////////
//******************** Transmit ACK of the received data ********************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//****************** Check header of incoming data message ******************//
///////////////////////////////////////////////////////////////////////////////
static void check_header_incoming_data_task(void *pvParameters) {
  char *header_hex_MSB = NULL;
  char *header_hex_LSB = NULL;

  uint8_t in_msg_type_tmp = 0;
  while (1) {
    // Block to wait for data received (+RCV=) and check if ACK
    // Block indefinitely (without a timeout, so no need to check the function's
    // return value) to wait for a notification. Here the RTOS task notification
    // is being used as a binary semaphore, so the notification value is cleared
    // to zero on exit. NOTE! Real applications should not block indefinitely,
    // but instead time out occasionally in order to handle error conditions
    // that may prevent the interrupt from sending any more notifications.
    ulTaskNotifyTake(pdTRUE,         // Clear the notification value on exit
                     portMAX_DELAY); // Block indefinitely

    // Print out remaining task stack memory (words) ************************
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    ESP_LOGW(TAG, "'check_header_incoming_data_task': <%zu>",
             uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGE(TAG, "**************** BYTES FREE IN TASK STACK ****************");
    // Print out remaining task stack memory (words) ************************

    /* ESP_LOGW(TAG,
             "***DEBUGGING*** Inside 'check_header_incoming_data_task' -> "
             "Lora_data.Data: <%s>",
             Lora_data.Data); */

    ///// extract the header of the incoming data ******************************
    // allocating memory for the header hexadecimal string
    // ALWAYS 4 digits: 0xAAAA
    header_hex_MSB = malloc((2 + 1) * sizeof(*header_hex_MSB));
    if (header_hex_MSB == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *header_hex_MSB in task "
                    "check_header_incoming_data_task");
    }
    //
    header_hex_LSB = malloc((2 + 1) * sizeof(*header_hex_LSB));
    if (header_hex_LSB == NULL) {
      ESP_LOGE(TAG, "NOT ENOUGH HEAP");
      ESP_LOGE(TAG, "Failed to allocate *header_hex_LSB in task "
                    "check_header_incoming_data_task");
    }
    GetSubString(Lora_data.Data, 0, 2, header_hex_MSB);
    GetSubString(Lora_data.Data, 2, 2, header_hex_LSB);
    // ESP_LOGW(TAG, "***DEBUGGING*** header_hex_MSB: <%s>", header_hex_MSB);
    // ESP_LOGW(TAG, "***DEBUGGING*** header_hex_LSB: <%s>", header_hex_LSB);
    //
    uint8_t header_dec_MSB = HexadecimalToDecimal(header_hex_MSB);
    uint8_t header_dec_LSB = HexadecimalToDecimal(header_hex_LSB);
    // ESP_LOGW(TAG, "***DEBUGGING*** header_dec_MSB: <%u>", header_dec_MSB);
    // ESP_LOGW(TAG, "***DEBUGGING*** header_dec_LSB: <%u>", header_dec_LSB);
    //
    free(header_hex_MSB);
    free(header_hex_LSB);
    ///// extract the header of the incoming data ******************************

    // combine 'header_dec_MSB' and 'header_dec_LSB' into only one variable so
    // it's easy to compare the last 14 bits corresponding with the transaction
    // ID of the incoming message
    uint16_t in_message_header_dec;
    in_message_header_dec = (header_dec_MSB << 8) & 0xFF00;
    in_message_header_dec |= header_dec_LSB;

    // check if the message type is 'ctrl'
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7|
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // | T |       Transaction ID      |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // we make an AND operation with '1100 0000 0000 0000' (0xC000)
    // and shift 14 spaces to the right
    in_msg_type_tmp = (in_message_header_dec & 0xC000) >> 14;
    switch (in_msg_type_tmp) {
    case 0:
      in_msg_type = rst; // 0000 0000
      break;
    case 1:
      in_msg_type = ctrl; // 0000 0001
      break;
    case 2:
      in_msg_type = data; // 0000 0010
      break;
    case 3:
      in_msg_type = ack; // 0000 0011
      break;
    default:
      break;
    }
    if (in_msg_type == ctrl) {
      // 30 characters to hold from the 4th character on Lora_data.Data
      in_ctrl_msg = malloc((30 + 1) * sizeof(*header_hex_MSB));
      if (in_ctrl_msg == NULL) {
        ESP_LOGE(TAG, "NOT ENOUGH HEAP");
        ESP_LOGE(TAG, "Failed to allocate *in_ctrl_msg in task "
                      "check_header_incoming_data_task");
      }
      //
      strcpy(in_ctrl_msg, Lora_data.Data + 4);

      xTaskNotifyGive(extract_info_from_ctrl_msg_task_handle);
    }

    // extract the transaction ID from the incoming header
    uint16_t in_transaction_ID_dec = 0;
    in_transaction_ID_dec = in_message_header_dec & 0x3FFF;
    ESP_LOGW(TAG, "***DEBUGGING*** Transaction ID of incoming message: <%u>",
             in_transaction_ID_dec);

    // we only send ack if the transaction ID of the incoming data is equal to
    // the message ID we are expecting
    // the message ID we are expecting is MSG_COUNTER_RX

    // if in_transaction_ID_dec == MSG_COUNTER_RX
    // it means we correctly received the message we were expecting
    if ((in_transaction_ID_dec == MSG_COUNTER_RX) ||
        (in_transaction_ID_dec == MSG_COUNTER_RX - 1)) {
      // so it means we received the message we were expecting
      // we set is_sending_ack flag to "Y" to know that
      // we are sending ACK message thru lora
      is_sending_ack = "Y";

      ///// VISUALIZE
      ///**********************************************************
      ESP_LOGE(TAG,
               "******************** FREE HEAP MEMORY ********************");
      ESP_LOGW(TAG, "<%lu> BYTES", xPortGetFreeHeapSize());
      ESP_LOGE(TAG,
               "******************** FREE HEAP MEMORY ********************");
      ///// VISUALIZE
      ///**********************************************************

      gpio_set_level(LED_PIN, 1);
      vTaskDelay(pdMS_TO_TICKS(100));
      gpio_set_level(LED_PIN, 0);
      vTaskDelay(pdMS_TO_TICKS(100));

      // if incoming_message_transaction_ID_dec == MSG_COUNTER_RX - 1 it
      // means we received a duplicated message because the ACK was not
      // received on the other side, so we still need to keep sending the ack
      // to the other side so he knows we already received that previous
      // message
      is_duplicated_data = in_transaction_ID_dec == MSG_COUNTER_RX ? "N" : "Y";

      ESP_LOGW(TAG, "Waiting 500ms to transmit 'ack' message\n");
      vTaskDelay(pdMS_TO_TICKS(DELAY));
      xTaskNotifyGive(transmit_ack_task_handle);
    }
  }
}
///////////////////////////////////////////////////////////////////////////////
//****************** Check header of incoming data message
//******************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//**************** Task to receeive the data in the UART port
//***************//
///////////////////////////////////////////////////////////////////////////////
// Data event is generated when data is copied out of the fifo and so it is
// based on hardware fifo size (128) with margin to prevent overflow
// Check for timeout_flag in data entry to recognize end of frame
static void uart_task(void *pvParameters) {
  uart_event_t event;

  // data received in uart port which is what we receive from LoRa
  // max 120 bytes
  uint8_t *incoming_uart_data = (uint8_t *)malloc(INCOMING_UART_DATA_SIZE);

  // to store each 120 bytes block we receive from UART port
  uint8_t *full_in_uart_data = (uint8_t *)malloc(FULL_IN_UART_DATA_SIZE);

  // to keep track of the UART sub-block of data received, 120 bytes max each
  uint8_t data_received_count = 0;

  while (1) {
    // waiting for UART event
    if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY)) {

      /* ESP_LOGE(TAG,
               "******************** <MEMORY CHECKING>
      *********************"); ESP_LOGI(TAG, "Heap integrety %i",
               heap_caps_check_integrity_all(true));              // Added
      ESP_LOGI(TAG, "FreeHeapSize: %lu", xPortGetFreeHeapSize()); // Added
      ESP_LOGI(TAG, "Bytes free in stack: %zu",
               uxTaskGetStackHighWaterMark(NULL)); // Added
      ESP_LOGE(
          TAG,
          "******************** <MEMORY CHECKING> *********************\n");
    */

      // ESP_LOGW(TAG, "***DEBUGGING*** ENTERING 'xQueueReceive' in
      // 'uart_task'");

      bzero(incoming_uart_data, INCOMING_UART_DATA_SIZE);

      // Print out remaining task stack memory (words)
      // ************************
      ESP_LOGE(TAG,
               "**************** BYTES FREE IN TASK STACK ****************");
      ESP_LOGW(TAG, "'uart_task': <%zu>", uxTaskGetStackHighWaterMark(NULL));
      ESP_LOGE(TAG,
               "**************** BYTES FREE IN TASK STACK ****************");
      // Print out remaining task stack memory (words)
      // ************************

      switch (event.type) {
        // Event of UART receving data
        /*We'd better handler data event fast, there would be much more data
        events than other types of events. If we take too much time on data
        event, the queue might be full.*/
      case UART_DATA:
        uart_read_bytes(UART_NUM, incoming_uart_data, event.size,
                        pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Data received from LoRa module: %s", incoming_uart_data);
        ESP_LOGI(TAG, "Length of data received: event.size = %zu", event.size);

        ///// if the module answers +OK and we are sending data
        ///****************
        if ((strncmp((const char *)incoming_uart_data, "+OK", 3) == 0) &&
            (strncmp((const char *)is_sending_ack, "Y", 1) == 0)) {
          // so we already sent ack, we put the flag back to "N"
          is_sending_ack = "N";

          // we increment counter to be waiting for the next incoming data
          // only if we received "+OK" from the lora module telling us we
          // successfully sent the ack, and if the data we received is not a
          // duplicated message
          if (strncmp((const char *)is_duplicated_data, "N", 1) == 0) {
            MSG_COUNTER_RX++;
          }

          ESP_LOGW(TAG, "***DEBUGGING*** MSG_COUNTER_RX: <%u>", MSG_COUNTER_RX);
        }
        ///// if the module answers +OK and we are sending data
        ///****************

        ///// if the module is receiving data, we proccess it
        ///******************
        // but only if the module is fully configured
        if (((strncmp((const char *)incoming_uart_data, "+RCV=", 5) == 0) ||
             (start_uart_block)) &&
            (strncmp((const char *)is_rylr998_module_init, "Y", 1) == 0)) {
          // we mark the start of the block
          start_uart_block = true;

          // verify if there is enough space in full_in_uart_data
          if (data_received_count + event.size <= FULL_IN_UART_DATA_SIZE) {
            // copy received data to full_in_uart_data
            memcpy(full_in_uart_data + data_received_count, incoming_uart_data,
                   event.size);
            // update the amount of data received
            data_received_count += event.size;
            ESP_LOGW(TAG,
                     "***DEBUGGING*** Inside 'if (data_received_count + "
                     "event.size "
                     "<= FULL_IN_UART_DATA_SIZE)' -> data_received_count: %u",
                     data_received_count);
          } else {
            ESP_LOGE(TAG, "There's no enough space in 'full_in_uart_data' to "
                          "store received data");
          }

          // if 'event.size == 120' it means we don't have a full block of
          // info, so we need to wait until we have the full block so the
          // programm won't enter this section
          if (event.size != 120) {
            full_in_uart_data[data_received_count] = '\0';

            ESP_LOGW(TAG, "***DEBUGGING*** full_in_uart_data: %s",
                     full_in_uart_data);
            ESP_LOGW(TAG, "***DEBUGGING*** data_received_count: %u",
                     data_received_count);

            // +RCV=22,length,data,RSSI,SNR
            // extract the components of the received message
            char *token = strtok((char *)full_in_uart_data, "=");
            // loop through the string to extract all other tokens
            uint8_t count_token = 0;
            while (token != NULL) {
              token = strtok(NULL, ",");
              count_token++;
              switch (count_token) {
              case 1:
                Lora_data.Address = token;
                break;
              case 2:
                Lora_data.DataLength = token;
                break;
              case 3:
                Lora_data.Data =
                    strdup(token); // strdup crea una copia de la cadena
                Lora_data.Data[strlen(Lora_data.Data)] = '\0';
                ESP_LOGW(TAG, "***DEBUGGING*** Lora_data.Data: <%s>",
                         Lora_data.Data);
                break;
              case 4:
                Lora_data.SignalStrength = token;
                break;
              case 5:
                Lora_data.SignalNoise = token;

                // send a notification to check_header_incoming_data_task,
                // bringing it out of the 'Blocked' state
                vTaskDelay(pdMS_TO_TICKS(DELAY / 10));
                xTaskNotifyGive(check_header_incoming_data_task_handle);
                break;
              default:
                break;
              }
            } // (token != NULL)
            // zero out
            bzero(full_in_uart_data, FULL_IN_UART_DATA_SIZE);
            data_received_count = 0;
            //
            // we mark the end of the block
            start_uart_block = false;
            //
          } // if (event.size != 120)
        }   // if ((strncmp((const char *)incoming_uart_data, "+RCV=", 5) == 0)
          // && (strncmp((const char *)is_rylr998_module_init, "Y", 1) == 0))
        ///// if the module is receiving data, we proccess it
        ///******************
        break;

      // Event of HW FIFO overflow detected
      case UART_FIFO_OVF:
        ESP_LOGI(TAG, "hw fifo overflow");
        // If fifo overflow happened, you should consider adding flow control
        // for your application. The ISR has already reset the rx FIFO, As an
        // example, we directly flush the rx buffer here in order to read more
        // data.
        uart_flush_input(UART_NUM);
        xQueueReset(uart_queue);
        break;

      // Event of UART ring buffer full
      case UART_BUFFER_FULL:
        ESP_LOGI(TAG, "ring buffer full");
        // If buffer full happened, you should consider increasing your buffer
        // size As an example, we directly flush the rx buffer here in order
        // to read more data.
        uart_flush_input(UART_NUM);
        xQueueReset(uart_queue);
        break;

      // Others
      default:
        ESP_LOGI(TAG, "UART event type: %d", event.type);
        break;
        ;
      }
    }
  }
  free(incoming_uart_data);
  free(full_in_uart_data);
  incoming_uart_data = NULL;
  vTaskDelete(NULL);
}
///////////////////////////////////////////////////////////////////////////////
//**************** Task to receeive the data in the UART port
//***************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//*************************** UART Initialization
//***************************//
///////////////////////////////////////////////////////////////////////////////
void init_uart(void) {
  uart_config_t uart_config = {
      .baud_rate = RYLR998_UART_BAUD_RATE,
      .data_bits = RYLR998_UART_DATA_BITS,
      .parity = RYLR998_UART_PARITY,
      .stop_bits = RYLR998_UART_STOP_BITS,
      .flow_ctrl = RYLR998_UART_FLOW_CTRL,
      .source_clk = RYLR998_UART_SOURCE_CLK,
  };

  // Configure UART parameters
  // uart_param_config(uart_port_t uart_num, const uart_config_t
  // *uart_config);
  ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));

  // Set UART pins
  // uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num,
  // int rts_io_num, int cts_io_num);
  // When calling 'uart_set_pin', instead of GPIO number, `UART_PIN_NO_CHANGE`
  // can be provided to keep the currently allocated pin.
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM, RYLR998_UART_TX_GPIO_NUM,
                               RYLR998_UART_RX_GPIO_NUM, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));

  // Install UART driver
  ESP_ERROR_CHECK(
      uart_driver_install(UART_NUM, BUF_SIZE, BUF_SIZE, 20, &uart_queue, 0));

  ESP_LOGI(TAG, "UART Interface Configuration !!!COMPLETED!!!");
}
///////////////////////////////////////////////////////////////////////////////
//*************************** UART Initialization
//***************************//
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
//******************************** Main Task
//********************************//
///////////////////////////////////////////////////////////////////////////////
void app_main(void) {
  init_led();
  ESP_LOGI(TAG, "Waiting 50 ms");
  vTaskDelay(pdMS_TO_TICKS(50));

  init_uart();
  ESP_LOGI(TAG, "Waiting 50 ms");
  vTaskDelay(pdMS_TO_TICKS(50));
  xTaskCreate(uart_task, "uart_task", TASK_MEMORY * 2, NULL, 12, NULL);
  ESP_LOGI(TAG, "Task 'uart_task' !!!CREATED!!!");
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");
  ESP_LOGW(TAG, "After 'uart_task' created");
  ESP_LOGW(TAG, "<%lu> BYTES", xPortGetFreeHeapSize());
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");

  //************************************************************************//
  ///// tasks creation /////
  xTaskCreatePinnedToCore(check_header_incoming_data_task,
                          "check_header_incoming_data_task", TASK_MEMORY * 2,
                          NULL, 10, &check_header_incoming_data_task_handle,
                          tskNO_AFFINITY);
  ESP_LOGI(TAG, "Task 'check_header_incoming_data_task' !!!CREATED!!!");
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");
  ESP_LOGW(TAG, "After 'check_header_incoming_data_task' created");
  ESP_LOGW(TAG, "<%lu> BYTES", xPortGetFreeHeapSize());
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");

  xTaskCreatePinnedToCore(transmit_ack_task, "transmit_ack_task",
                          TASK_MEMORY * 2, NULL, 10, &transmit_ack_task_handle,
                          tskNO_AFFINITY);
  ESP_LOGI(TAG, "Task 'transmit_ack_task' !!!CREATED!!!");
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");
  ESP_LOGW(TAG, "After 'transmit_ack_task' created");
  ESP_LOGW(TAG, "<%lu> BYTES", xPortGetFreeHeapSize());
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");

  xTaskCreatePinnedToCore(extract_info_from_ctrl_msg_task,
                          "extract_info_from_ctrl_msg_task", TASK_MEMORY * 2,
                          NULL, 10, &extract_info_from_ctrl_msg_task_handle,
                          tskNO_AFFINITY);
  ESP_LOGI(TAG, "Task 'extract_info_from_ctrl_msg_task' !!!CREATED!!!");
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");
  ESP_LOGW(TAG, "After 'extract_info_from_ctrl_msg_task' created");
  ESP_LOGW(TAG, "<%lu> BYTES", xPortGetFreeHeapSize());
  ESP_LOGE(TAG, "******************** FREE HEAP MEMORY ********************");
  ///// tasks creation /////
  //************************************************************************//

  init_rylr998_module();
  ESP_LOGI(TAG, "Waiting 50 ms");
  vTaskDelay(pdMS_TO_TICKS(50));

  ESP_LOGI(TAG, "******************************************************");
  ESP_LOGI(TAG, "****************** READY TO RECEIVE ******************");
  ESP_LOGI(TAG, "******************************************************\n");
}
///////////////////////////////////////////////////////////////////////////////
//******************************** Main Task
//********************************//
///////////////////////////////////////////////////////////////////////////////