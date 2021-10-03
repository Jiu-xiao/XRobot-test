/*
  裁判系统抽象。
*/

/* Includes ----------------------------------------------------------------- */
#include "dev_referee.h"

#include <string.h>

#include "bsp_delay.h"
#include "bsp_uart.h"
#include "comp_crc16.h"
#include "comp_crc8.h"
#include "comp_utils.h"
#include "protocol.h"

/* Private define ----------------------------------------------------------- */
#define REF_HEADER_SOF (0xA5)
#define REF_LEN_RX_BUFF (0xFF)

// TODO:FREQ支持小数
#define REF_UI_FAST_REFRESH_FREQ (50) /* 静态元素刷新频率 */
#define REF_UI_SLOW_REFRESH_FREQ (1)  /* 动态元素刷新频率 */

#define REF_UI_BOX_UP_OFFSET (4)
#define REF_UI_BOX_BOT_OFFSET (-14)

#define REF_UI_RIGHT_START_W (0.85f)

#define REF_UI_MODE_LINE1_H (0.7f)
#define REF_UI_MODE_LINE2_H (0.68f)
#define REF_UI_MODE_LINE3_H (0.66f)
#define REF_UI_MODE_LINE4_H (0.64f)

#define REF_UI_MODE_OFFSET_1_LEFT (-6)
#define REF_UI_MODE_OFFSET_1_RIGHT (44)
#define REF_UI_MODE_OFFSET_2_LEFT (54)
#define REF_UI_MODE_OFFSET_2_RIGHT (102)
#define REF_UI_MODE_OFFSET_3_LEFT (114)
#define REF_UI_MODE_OFFSET_3_RIGHT (162)
#define REF_UI_MODE_OFFSET_4_LEFT (174)
#define REF_UI_MODE_OFFSET_4_RIGHT (222)

/* Private macro ------------------------------------------------------------ */
/* Private typedef ---------------------------------------------------------- */

typedef struct __packed {
  Referee_Header_t header;
  uint16_t cmd_id;
  Referee_InterStudentHeader_t student_header;
} Referee_UiPacketHead_t;

/* Private variables -------------------------------------------------------- */

static uint8_t rxbuf[REF_LEN_RX_BUFF];

static Referee_t *gref;

static bool inited = false;

/* Private function  -------------------------------------------------------- */

static void Referee_RxCpltCallback(void) {
  BaseType_t switch_required;
  xTaskNotifyFromISR(gref->thread_alert, SIGNAL_REFEREE_RAW_REDY,
                     eSetValueWithOverwrite, &switch_required);
  portYIELD_FROM_ISR(switch_required);
}

static void Referee_IdleLineCallback(void) {
  HAL_UART_AbortReceive_IT(BSP_UART_GetHandle(BSP_UART_REF));
}

static void Referee_AbortRxCpltCallback(void) {
  BaseType_t switch_required;
  xTaskNotifyFromISR(gref->thread_alert, SIGNAL_REFEREE_RAW_REDY,
                     eSetValueWithOverwrite, &switch_required);
  portYIELD_FROM_ISR(switch_required);
}

static void RefereeFastRefreshTimerCallback(void *arg) {
  RM_UNUSED(arg);
  xTaskNotify(gref->thread_alert, SIGNAL_REFEREE_FAST_REFRESH_UI,
              eSetValueWithOverwrite);
}

static void RefereeSlowRefreshTimerCallback(void *arg) {
  RM_UNUSED(arg);
  xTaskNotify(gref->thread_alert, SIGNAL_REFEREE_SLOW_REFRESH_UI,
              eSetValueWithOverwrite);
}

static int8_t Referee_SetPacketHeader(Referee_Header_t *header,
                                      uint16_t data_length) {
  header->sof = REF_HEADER_SOF;
  header->data_length = data_length;
  header->seq = 0;
  header->crc8 =
      CRC8_Calc((const uint8_t *)header,
                sizeof(Referee_Header_t) - sizeof(uint8_t), CRC8_INIT);
  return DEVICE_OK;
}

static int8_t Referee_SetUiHeader(Referee_InterStudentHeader_t *header,
                                  const Referee_StudentCMDID_t cmd_id,
                                  Referee_RobotID_t robot_id) {
  header->cmd_id = cmd_id;
  header->id_sender = robot_id;
  if (robot_id > 100) {
    header->id_receiver = robot_id - 101 + 0x0165;
  } else {
    header->id_receiver = robot_id + 0x0100;
  }
  return DEVICE_OK;
}

/* Exported functions ------------------------------------------------------- */

int8_t Referee_Init(Referee_t *ref, const UI_Screen_t *screen) {
  ASSERT(ref);
  if (inited) return DEVICE_ERR_INITED;

  gref = ref;

  VERIFY((gref->thread_alert = xTaskGetCurrentTaskHandle()) != NULL);

  ref->ui.screen = screen;

  BSP_UART_RegisterCallback(BSP_UART_REF, BSP_UART_RX_CPLT_CB,
                            Referee_RxCpltCallback, NULL);
  BSP_UART_RegisterCallback(BSP_UART_REF, BSP_UART_ABORT_RX_CPLT_CB,
                            Referee_AbortRxCpltCallback, NULL);
  BSP_UART_RegisterCallback(BSP_UART_REF, BSP_UART_IDLE_LINE_CB,
                            Referee_IdleLineCallback, NULL);
  ref->ui_fast_timer_id =
      xTimerCreate("fast_refresh", pdMS_TO_TICKS(REF_UI_FAST_REFRESH_FREQ),
                   pdTRUE, NULL, RefereeFastRefreshTimerCallback);

  ref->ui_slow_timer_id =
      xTimerCreate("slow_refresh", pdMS_TO_TICKS(REF_UI_SLOW_REFRESH_FREQ),
                   pdTRUE, NULL, RefereeSlowRefreshTimerCallback);

  xTimerStart(ref->ui_fast_timer_id,
              pdMS_TO_TICKS(1000 / REF_UI_FAST_REFRESH_FREQ));
  xTimerStart(ref->ui_slow_timer_id,
              pdMS_TO_TICKS(1000 / REF_UI_SLOW_REFRESH_FREQ));

  __HAL_UART_ENABLE_IT(BSP_UART_GetHandle(BSP_UART_REF), UART_IT_IDLE);

  inited = true;
  return DEVICE_OK;
}

int8_t Referee_Restart(void) {
  __HAL_UART_DISABLE(BSP_UART_GetHandle(BSP_UART_REF));
  __HAL_UART_ENABLE(BSP_UART_GetHandle(BSP_UART_REF));
  return DEVICE_OK;
}

void Referee_HandleOffline(Referee_t *ref) { ref->status = REF_STATUS_OFFLINE; }

int8_t Referee_StartReceiving(Referee_t *ref) {
  RM_UNUSED(ref);
  if (HAL_UART_Receive_DMA(BSP_UART_GetHandle(BSP_UART_REF), rxbuf,
                           REF_LEN_RX_BUFF) == HAL_OK) {
    return DEVICE_OK;
  }
  return DEVICE_ERR;
}

bool Referee_WaitRecvCplt(uint32_t timeout) {
  return xTaskNotifyWait(0, 0, SIGNAL_REFEREE_RAW_REDY, pdMS_TO_TICKS(timeout));
}

int8_t Referee_Parse(Referee_t *ref) {
  ref->status = REF_STATUS_RUNNING;
  uint32_t data_length =
      REF_LEN_RX_BUFF -
      __HAL_DMA_GET_COUNTER(BSP_UART_GetHandle(BSP_UART_REF)->hdmarx);

  const uint8_t *index = rxbuf; /* const 保护原始rxbuf不被修改 */
  const uint8_t *const rxbuf_end = rxbuf + data_length;

  while (index < rxbuf_end) {
    /* 1.处理帧头 */
    /* 1.1遍历所有找到SOF */
    while ((*index != REF_HEADER_SOF) && (index < rxbuf_end)) {
      index++;
    }
    /* 1.2将剩余数据当做帧头部 */
    Referee_Header_t *header = (Referee_Header_t *)index;

    /* 1.3验证完整性 */
    if (!CRC8_Verify((uint8_t *)header, sizeof(*header))) continue;
    index += sizeof(*header);

    /* 2.处理CMD ID */
    /* 2.1将剩余数据当做CMD ID处理 */
    Referee_CMDID_t *cmd_id = (Referee_CMDID_t *)index;
    index += sizeof(*cmd_id);

    /* 3.处理数据段 */
    void *destination;
    size_t size;

    switch (*cmd_id) {
      case REF_CMD_ID_GAME_STATUS:
        destination = &(ref->game_status);
        size = sizeof(ref->game_status);
        break;
      case REF_CMD_ID_GAME_RESULT:
        destination = &(ref->game_result);
        size = sizeof(ref->game_result);
        break;
      case REF_CMD_ID_GAME_ROBOT_HP:
        destination = &(ref->game_robot_hp);
        size = sizeof(ref->game_robot_hp);
        break;
      case REF_CMD_ID_DART_STATUS:
        destination = &(ref->dart_status);
        size = sizeof(ref->dart_status);
        break;
      case REF_CMD_ID_ICRA_ZONE_STATUS:
        destination = &(ref->icra_zone);
        size = sizeof(ref->icra_zone);
        break;
      case REF_CMD_ID_FIELD_EVENTS:
        destination = &(ref->field_event);
        size = sizeof(ref->field_event);
        break;
      case REF_CMD_ID_SUPPLY_ACTION:
        destination = &(ref->supply_action);
        size = sizeof(ref->supply_action);
        break;
      case REF_CMD_ID_WARNING:
        destination = &(ref->warning);
        size = sizeof(ref->warning);
        break;
      case REF_CMD_ID_DART_COUNTDOWN:
        destination = &(ref->dart_countdown);
        size = sizeof(ref->dart_countdown);
        break;
      case REF_CMD_ID_ROBOT_STATUS:
        destination = &(ref->robot_status);
        size = sizeof(ref->robot_status);
        break;
      case REF_CMD_ID_POWER_HEAT_DATA:
        destination = &(ref->power_heat);
        size = sizeof(ref->power_heat);
        break;
      case REF_CMD_ID_ROBOT_POS:
        destination = &(ref->robot_pos);
        size = sizeof(ref->robot_pos);
        break;
      case REF_CMD_ID_ROBOT_BUFF:
        destination = &(ref->robot_buff);
        size = sizeof(ref->robot_buff);
        break;
      case REF_CMD_ID_DRONE_ENERGY:
        destination = &(ref->drone_energy);
        size = sizeof(ref->drone_energy);
        break;
      case REF_CMD_ID_ROBOT_DMG:
        destination = &(ref->robot_danage);
        size = sizeof(ref->robot_danage);
        break;
      case REF_CMD_ID_LAUNCHER_DATA:
        destination = &(ref->launcher_data);
        size = sizeof(ref->launcher_data);
        break;
      case REF_CMD_ID_BULLET_REMAINING:
        destination = &(ref->bullet_remain);
        size = sizeof(ref->bullet_remain);
        break;
      case REF_CMD_ID_RFID:
        destination = &(ref->rfid);
        size = sizeof(ref->rfid);
        break;
      case REF_CMD_ID_DART_CLIENT:
        destination = &(ref->dart_client);
        size = sizeof(ref->dart_client);
        break;
      case REF_CMD_ID_CLIENT_MAP:
        destination = &(ref->client_map);
        size = sizeof(ref->client_map);
        break;
      case REF_CMD_ID_KEYBOARD_MOUSE:
        destination = &(ref->keyboard_mouse);
        size = sizeof(ref->keyboard_mouse);
        break;
      default:
        return DEVICE_ERR;
    }
    index += size;

    /* 4.处理帧尾 */
    index += sizeof(Referee_Tail_t);

    /* 验证无误则接受数据 */
    if (CRC16_Verify((uint8_t *)header, (uint8_t)(index - (uint8_t *)header)))
      memcpy(destination, index, size);
  }
  return DEVICE_OK;
}

uint8_t Referee_RefreshUI(Referee_t *ref) {
  UI_Ele_t ele;
  UI_String_t string;

  const float kW = ref->ui.screen->width;
  const float kH = ref->ui.screen->height;

  float box_pos_left = 0.0f, box_pos_right = 0.0f;

  static UI_GraphicOp_t graphic_op = UI_GRAPHIC_OP_ADD;

  /* UI动态元素刷新 */
  uint32_t flag;
  xTaskNotifyWait(0, 0, flag, 0);
  if (flag & SIGNAL_REFEREE_FAST_REFRESH_UI) {
    /* 使用状态机算法，每次更新一个图层 */
    switch (ref->ui.refresh_fsm) {
      case 0: {
        ref->ui.refresh_fsm++;

        /* 更新云台底盘相对方位 */
        const float kLEN = 22;
        UI_DrawLine(
            &ele, "6", graphic_op, UI_GRAPHIC_LAYER_CHASSIS, UI_GREEN,
            UI_DEFAULT_WIDTH * 12, (uint16_t)(kW * 0.4f), (uint16_t)(kH * 0.2f),
            (uint16_t)(kW * 0.4f + sinf(ref->chassis_ui.angle) * 2 * kLEN),
            (uint16_t)(kH * 0.2f + cosf(ref->chassis_ui.angle) * 2 * kLEN));

        UI_StashGraphic(&(ref->ui), &ele);

        /* 更新底盘模式选择框 */
        switch (ref->chassis_ui.mode) {
          case CHASSIS_MODE_FOLLOW_GIMBAL:
            box_pos_left = REF_UI_MODE_OFFSET_2_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_2_RIGHT;
            break;
          case CHASSIS_MODE_FOLLOW_GIMBAL_35:
            box_pos_left = REF_UI_MODE_OFFSET_3_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_3_RIGHT;
            break;
          case CHASSIS_MODE_ROTOR:
            box_pos_left = REF_UI_MODE_OFFSET_4_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_4_RIGHT;
            break;
          default:
            box_pos_left = 0.0f;
            box_pos_right = 0.0f;
            break;
        }
        if (box_pos_left != 0.0f && box_pos_right != 0.0f) {
          UI_DrawRectangle(
              &ele, "8", graphic_op, UI_GRAPHIC_LAYER_CHASSIS, UI_GREEN,
              UI_DEFAULT_WIDTH,
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_left),
              (uint16_t)(kH * REF_UI_MODE_LINE1_H + REF_UI_BOX_UP_OFFSET),
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_right),
              (uint16_t)(kH * REF_UI_MODE_LINE1_H + REF_UI_BOX_BOT_OFFSET));

          UI_StashGraphic(&(ref->ui), &ele);
        }
        break;
      }
      case 1:
        ref->ui.refresh_fsm++;
        /* 更新电容状态 */
        if (ref->cap_ui.online) {
          UI_DrawArc(&ele, "9", graphic_op, UI_GRAPHIC_LAYER_CAP, UI_GREEN, 0,
                     (uint16_t)(ref->cap_ui.percentage * 360.f),
                     UI_DEFAULT_WIDTH * 5, (uint16_t)(kW * 0.6f),
                     (uint16_t)(kH * 0.2f), 50, 50);
        } else {
          UI_DrawArc(&ele, "9", graphic_op, UI_GRAPHIC_LAYER_CAP, UI_YELLOW, 0,
                     360, UI_DEFAULT_WIDTH * 5, (uint16_t)(kW * 0.6f),
                     (uint16_t)(kH * 0.2), 50, 50);
        }
        UI_StashGraphic(&(ref->ui), &ele);
        break;
      case 2: {
        ref->ui.refresh_fsm++;

        /* 更新云台模式选择框 */
        switch (ref->gimbal_ui.mode) {
          case GIMBAL_MODE_RELAX:
            box_pos_left = REF_UI_MODE_OFFSET_2_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_2_RIGHT;
            break;
          case GIMBAL_MODE_ABSOLUTE:
            box_pos_left = REF_UI_MODE_OFFSET_3_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_3_RIGHT;
            break;
          case GIMBAL_MODE_RELATIVE:
            box_pos_left = REF_UI_MODE_OFFSET_4_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_4_RIGHT;
            break;
          default:
            box_pos_left = 0.0f;
            box_pos_right = 0.0f;
            break;
        }
        if (box_pos_left != 0.0f && box_pos_right != 0.0f) {
          UI_DrawRectangle(
              &ele, "a", graphic_op, UI_GRAPHIC_LAYER_GIMBAL, UI_GREEN,
              UI_DEFAULT_WIDTH,
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_left),
              (uint16_t)(kH * REF_UI_MODE_LINE2_H + REF_UI_BOX_UP_OFFSET),
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_right),
              (uint16_t)(kH * REF_UI_MODE_LINE2_H + REF_UI_BOX_BOT_OFFSET));
          UI_StashGraphic(&(ref->ui), &ele);
        }
        break;
      }
      case 3: {
        ref->ui.refresh_fsm++;

        /* 更新发射器模式选择框 */
        switch (ref->launcher_ui.mode) {
          case LAUNCHER_MODE_RELAX:
            box_pos_left = REF_UI_MODE_OFFSET_2_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_2_RIGHT;
            break;
          case LAUNCHER_MODE_SAFE:
            box_pos_left = REF_UI_MODE_OFFSET_3_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_3_RIGHT;
            break;
          case LAUNCHER_MODE_LOADED:
            box_pos_left = REF_UI_MODE_OFFSET_4_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_4_RIGHT;
            break;
          default:
            box_pos_left = 0.0f;
            box_pos_right = 0.0f;
            break;
        }
        if (box_pos_left != 0.0f && box_pos_right != 0.0f) {
          UI_DrawRectangle(
              &ele, "b", graphic_op, UI_GRAPHIC_LAYER_LAUNCHER, UI_GREEN,
              UI_DEFAULT_WIDTH,
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_left),
              (uint16_t)(kH * REF_UI_MODE_LINE3_H + REF_UI_BOX_UP_OFFSET),
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_right),
              (uint16_t)(kH * REF_UI_MODE_LINE3_H + REF_UI_BOX_BOT_OFFSET));
          UI_StashGraphic(&(ref->ui), &ele);
        }

        /* 更新开火模式选择框 */
        switch (ref->launcher_ui.fire) {
          case FIRE_MODE_SINGLE:
            box_pos_left = REF_UI_MODE_OFFSET_2_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_2_RIGHT;
            break;
          case FIRE_MODE_BURST:
            box_pos_left = REF_UI_MODE_OFFSET_3_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_3_RIGHT;
            break;
          case FIRE_MODE_CONT:
            box_pos_left = REF_UI_MODE_OFFSET_4_LEFT;
            box_pos_right = REF_UI_MODE_OFFSET_4_RIGHT;
            break;
          default:
            box_pos_left = 0.0f;
            box_pos_right = 0.0f;
            break;
        }
        if (box_pos_left != 0.0f && box_pos_right != 0.0f) {
          UI_DrawRectangle(
              &ele, "f", graphic_op, UI_GRAPHIC_LAYER_LAUNCHER, UI_GREEN,
              UI_DEFAULT_WIDTH,
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_left),
              (uint16_t)(kH * REF_UI_MODE_LINE4_H + REF_UI_BOX_UP_OFFSET),
              (uint16_t)(kW * REF_UI_RIGHT_START_W + box_pos_right),
              (uint16_t)(kH * REF_UI_MODE_LINE4_H + REF_UI_BOX_BOT_OFFSET));
          UI_StashGraphic(&(ref->ui), &ele);
        }
        break;
      }
      case 4:
        ref->ui.refresh_fsm++;

        switch (ref->cmd_ui.ctrl_method) {
          case CMD_METHOD_MOUSE_KEYBOARD:
            UI_DrawRectangle(&ele, "c", graphic_op, UI_GRAPHIC_LAYER_CMD,
                             UI_GREEN, UI_DEFAULT_WIDTH,
                             (uint16_t)(kW * REF_UI_RIGHT_START_W + 96.f),
                             (uint16_t)(kH * 0.4f + REF_UI_BOX_UP_OFFSET),
                             (uint16_t)(kW * REF_UI_RIGHT_START_W + 120.f),
                             (uint16_t)(kH * 0.4f + REF_UI_BOX_BOT_OFFSET));
            break;
          case CMD_METHOD_JOYSTICK_SWITCH:
            UI_DrawRectangle(&ele, "c", graphic_op, UI_GRAPHIC_LAYER_CMD,
                             UI_GREEN, UI_DEFAULT_WIDTH,
                             (uint16_t)(kW * REF_UI_RIGHT_START_W + 56.f),
                             (uint16_t)(kH * 0.4f + REF_UI_BOX_UP_OFFSET),
                             (uint16_t)(kW * REF_UI_RIGHT_START_W + 80.f),
                             (uint16_t)(kH * 0.4f + REF_UI_BOX_BOT_OFFSET));
            break;
        }
        UI_StashGraphic(&(ref->ui), &ele);
        break;

      default:
        ref->ui.refresh_fsm = 0;
    }

    if (graphic_op == UI_GRAPHIC_OP_ADD && ref->ui.refresh_fsm == 1)
      graphic_op = UI_GRAPHIC_OP_REWRITE;
  }

  /* UI静态元素刷新 */
  if (flag & SIGNAL_REFEREE_SLOW_REFRESH_UI) {
    graphic_op = UI_GRAPHIC_OP_ADD;
    ref->ui.refresh_fsm = 1;

    osThreadFlagsClear(SIGNAL_REFEREE_SLOW_REFRESH_UI);
    UI_DrawString(&string, "1", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 10, 80, UI_CHAR_DEFAULT_WIDTH,
                  (uint16_t)(kW * REF_UI_RIGHT_START_W),
                  (uint16_t)(kH * REF_UI_MODE_LINE1_H),
                  "CHAS  FLLW  FL35  ROTR");
    UI_StashString(&(ref->ui), &string);

    UI_DrawString(&string, "2", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 10, 80, UI_CHAR_DEFAULT_WIDTH,
                  (uint16_t)(kW * REF_UI_RIGHT_START_W),
                  (uint16_t)(kH * REF_UI_MODE_LINE2_H),
                  "GMBL  RELX  ABSL  RLTV");
    UI_StashString(&(ref->ui), &string);

    UI_DrawString(&string, "3", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 10, 80, UI_CHAR_DEFAULT_WIDTH,
                  (uint16_t)(kW * REF_UI_RIGHT_START_W),
                  (uint16_t)(kH * REF_UI_MODE_LINE3_H),
                  "SHOT  RELX  SAFE  LOAD");
    UI_StashString(&(ref->ui), &string);

    UI_DrawString(&string, "4", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 10, 80, UI_CHAR_DEFAULT_WIDTH,
                  (uint16_t)(kW * REF_UI_RIGHT_START_W),
                  (uint16_t)(kH * REF_UI_MODE_LINE4_H),
                  "FIRE  SNGL  BRST  CONT");
    UI_StashString(&(ref->ui), &string);

    UI_DrawLine(&ele, "5", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                UI_DEFAULT_WIDTH * 3, (uint16_t)(kW * 0.4f),
                (uint16_t)(kH * 0.2f), (uint16_t)(kW * 0.4f),
                (uint16_t)(kH * 0.2f + 50.f));
    UI_StashGraphic(&(ref->ui), &ele);

    UI_DrawString(&string, "d", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 10, 80, UI_CHAR_DEFAULT_WIDTH,
                  (uint16_t)(kW * REF_UI_RIGHT_START_W), (uint16_t)(kH * 0.4f),
                  "CTRL  JS  KM");
    UI_StashString(&(ref->ui), &string);

    UI_DrawString(&string, "e", graphic_op, UI_GRAPHIC_LAYER_CONST, UI_GREEN,
                  UI_DEFAULT_WIDTH * 20, 80, UI_CHAR_DEFAULT_WIDTH * 2,
                  (uint16_t)(kW * 0.6f - 26.0f), (uint16_t)(kH * 0.2f + 10.0f),
                  "CAP");
    UI_StashString(&(ref->ui), &string);
  }

  xTaskNotifyStateClear(xTaskGetCurrentTaskHandle());

  return DEVICE_OK;
}

/**
 * @brief 组装UI包
 *
 * @param ui UI数据
 * @param ref 裁判系统数据
 * @return int8_t 0代表成功
 */
int8_t Referee_PackUiPacket(Referee_t *ref) {
  UI_Ele_t *ele = NULL;
  UI_String_t string;
  UI_Del_t del;

  Referee_StudentCMDID_t ui_cmd_id;
  static const size_t kSIZE_DATA_HEADER = sizeof(Referee_InterStudentHeader_t);
  size_t size_data_content;
  static const size_t kSIZE_PACKET_CRC = sizeof(uint16_t);
  void *source = NULL;

  if (!UI_PopDel(&(ref->ui), &del)) {
    source = &del;
    size_data_content = sizeof(UI_Del_t);
    ui_cmd_id = REF_STDNT_CMD_ID_UI_DEL;
  } else if (ref->ui.stack.size.graphic) { /* 绘制图形 */
    if (ref->ui.stack.size.graphic <= 1) {
      size_data_content = sizeof(UI_Ele_t) * 1;
      ui_cmd_id = REF_STDNT_CMD_ID_UI_DRAW1;

    } else if (ref->ui.stack.size.graphic <= 2) {
      size_data_content = sizeof(UI_Ele_t) * 2;
      ui_cmd_id = REF_STDNT_CMD_ID_UI_DRAW2;

    } else if (ref->ui.stack.size.graphic <= 5) {
      size_data_content = sizeof(UI_Ele_t) * 5;
      ui_cmd_id = REF_STDNT_CMD_ID_UI_DRAW5;

    } else if (ref->ui.stack.size.graphic <= 7) {
      size_data_content = sizeof(UI_Ele_t) * 7;
      ui_cmd_id = REF_STDNT_CMD_ID_UI_DRAW7;

    } else {
      return DEVICE_ERR;
    }
    ele = BSP_Malloc(size_data_content);
    UI_Ele_t *cursor = ele;
    while (!UI_PopGraphic(&(ref->ui), cursor)) {
      cursor++;
    }
    source = ele;
  } else if (!UI_PopString(&(ref->ui), &string)) { /* 绘制字符 */
    source = &string;
    size_data_content = sizeof(UI_String_t);
    ui_cmd_id = REF_STDNT_CMD_ID_UI_STR;
  } else {
    return DEVICE_ERR;
  }

  ref->packet.size =
      sizeof(Referee_UiPacketHead_t) + size_data_content + kSIZE_PACKET_CRC;

  ref->packet.data = BSP_Malloc(ref->packet.size);

  Referee_UiPacketHead_t *packet_head =
      (Referee_UiPacketHead_t *)(ref->packet.data);

  Referee_SetPacketHeader(&(packet_head->header),
                          kSIZE_DATA_HEADER + (uint16_t)size_data_content);
  packet_head->cmd_id = REF_CMD_ID_INTER_STUDENT;
  Referee_SetUiHeader(&(packet_head->student_header), ui_cmd_id,
                      ref->robot_status.robot_id);
  memcpy(ref->packet.data + sizeof(Referee_UiPacketHead_t), source,
         size_data_content);

  BSP_Free(ele);
  uint16_t *crc =
      (uint16_t *)(ref->packet.data + ref->packet.size - kSIZE_PACKET_CRC);
  *crc = CRC16_Calc((const uint8_t *)ref->packet.data,
                    ref->packet.size - kSIZE_PACKET_CRC, CRC16_INIT);

  return DEVICE_OK;
}

int8_t Referee_StartTransmit(Referee_t *ref) {
  if (ref->packet.data == NULL) {
    xTaskNotify(gref->thread_alert, SIGNAL_REFEREE_PACKET_SENT,
                eSetValueWithOverwrite);
    return DEVICE_ERR_NULL;
  }
  if (HAL_UART_Transmit_DMA(BSP_UART_GetHandle(BSP_UART_REF), ref->packet.data,
                            (uint16_t)ref->packet.size) == HAL_OK) {
    BSP_Free(ref->packet.last_data);
    ref->packet.last_data = ref->packet.data;
    ref->packet.data = NULL;
    xTaskNotify(gref->thread_alert, SIGNAL_REFEREE_PACKET_SENT,
                eSetValueWithOverwrite);
    return DEVICE_OK;
  }
  return DEVICE_ERR;
}

bool Referee_WaitTransCplt(uint32_t timeout) {
  return xTaskNotifyWait(0, 0, SIGNAL_REFEREE_PACKET_SENT,
                         pdMS_TO_TICKS(timeout));
}

uint8_t Referee_PackForChassis(Referee_ForChassis_t *c_ref,
                               const Referee_t *ref) {
  c_ref->chassis_power_limit = ref->robot_status.chassis_power_limit;
  c_ref->chassis_pwr_buff = ref->power_heat.chassis_pwr_buff;
  c_ref->status = ref->status;
  return DEVICE_OK;
}

uint8_t Referee_PackForLauncher(Referee_ForLauncher_t *l_ref, Referee_t *ref) {
  memcpy(&(l_ref->power_heat), &(ref->power_heat), sizeof(l_ref->power_heat));
  memcpy(&(l_ref->robot_status), &(ref->robot_status),
         sizeof(l_ref->robot_status));
  memcpy(&(l_ref->launcher_data), &(ref->launcher_data),
         sizeof(l_ref->launcher_data));
  l_ref->status = ref->status;
  return DEVICE_OK;
}

uint8_t Referee_PackForCap(Referee_ForCap_t *cap_ref, const Referee_t *ref) {
  cap_ref->chassis_power_limit = ref->robot_status.chassis_power_limit;
  cap_ref->chassis_pwr_buff = ref->power_heat.chassis_pwr_buff;
  cap_ref->chassis_watt = ref->power_heat.chassis_watt;
  cap_ref->status = ref->status;
  return DEVICE_OK;
}

uint8_t Referee_PackForAI(Referee_ForAI_t *ai_ref, const Referee_t *ref) {
  if (ref->robot_status.robot_id < REF_BOT_BLU_HERO)
    ai_ref->team = AI_TEAM_RED;
  else
    ai_ref->team = AI_TEAM_BLUE;

  ai_ref->status = ref->status;
  return DEVICE_OK;
}