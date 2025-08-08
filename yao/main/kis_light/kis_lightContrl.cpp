#include "kis_lightContrl.h"
#include "../include/Device.h"
#include <stdio.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include <cstring> 
#include <math.h> 
 #define UART_PORT_NUM UART_NUM_1
 #define TXD_PIN GPIO_NUM_1
 #define RXD_PIN GPIO_NUM_0
 #define BUF_SIZE (1024)
static unsigned char controlData[60];
 void uart1Initr(void)
 {

    // // UART 配置
    uart_config_t uart_config = {
         .baud_rate = 115200,
         .data_bits = UART_DATA_8_BITS,
         .parity = UART_PARITY_DISABLE,
         .stop_bits =  UART_STOP_BITS_1,
         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
         .rx_flow_ctrl_thresh = 122,
         .source_clk = UART_SCLK_DEFAULT,
         .flags = 0 // 添加这一行
    };
     // 安装 UART 驱动
    uart_param_config(UART_PORT_NUM, &uart_config);

    // // 设置 UART 引脚
    uart_set_pin(UART_PORT_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // // 安装队列
    uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);

    // // 发送测试数据
    const char *test_str = "Hello from UART2 on ESP32!\n";
    uart_write_bytes(UART_PORT_NUM, test_str, strlen(test_str));
}
/*将 0~254 映射到 0~1000*/ 
int convert_brightness(int x) {
    if (x < 0 || x > 254) {
        return -1;  // 输入范围错误，返回 -1 表示异常
    }

    // 使用浮点运算，避免整数除法的问题
    double y = (double)x * 1000.0 / 254.0;
    return (int)round(y);  // 四舍五入后转为整数
}
void setLevelCallback(unsigned char levelValue,unsigned short lightEndpointId)
{
     Device*device = Device::GetDevice(lightEndpointId);
     unsigned char dataIndex = 0;
     unsigned short currentLevel = 0;
     if (device->IsReachable()) {
      device->SetLevel(levelValue);
      currentLevel=convert_brightness(levelValue);
      controlData[dataIndex++] = 0x07;
      controlData[dataIndex++] = 0x03;
      controlData[dataIndex++] = 0xea;
      controlData[dataIndex++] = 0x02;
      controlData[dataIndex++] = (currentLevel&0xff00)>>8;
      controlData[dataIndex++] = currentLevel&0xff;
      sendDataToZigbee(controlData,dataIndex,0x0240,0xffff);
      ChipLogProgress(DeviceLayer, "Set level temp to %d on endpoint %d (%s)", levelValue,lightEndpointId, device->GetName());
     }
}

unsigned char crc8Calculate_receive(unsigned short length, unsigned char *data){
	int n;
	unsigned char crc8 ;
	crc8  = (data[0]>> 0) & 0xff;
	for(n = 1; n < length; n++)	{
		crc8 ^= data[n];
	}
	return crc8;
}

     void sendDataToZigbee(unsigned char *dataAddr,unsigned char dataLen,unsigned short cluseCmd,unsigned short deviceEndpointId)
{
     unsigned char cmdArry[100];
     unsigned char  dataIndex = 0;
     memset(cmdArry,0,sizeof(cmdArry));
     cmdArry[dataIndex++] = 0x71;
     cmdArry[dataIndex++] = 0x01;
     cmdArry[dataIndex++] = (cluseCmd&0xff00)>>8;
     cmdArry[dataIndex++] = cluseCmd&0xff;
     cmdArry[dataIndex++] = 0XFF;
     cmdArry[dataIndex++] = 0XFf;
     cmdArry[dataIndex++] = (deviceEndpointId&0xff00)>>8;
     cmdArry[dataIndex++] = deviceEndpointId&0xff;
     cmdArry[dataIndex++] = (dataLen&0xff00)>>8;
     cmdArry[dataIndex++] = dataLen&0xff;
     memcpy(&cmdArry[dataIndex],dataAddr,dataLen);
     dataIndex += dataLen;
     cmdArry[dataIndex] = crc8Calculate_receive(dataIndex,cmdArry);
     dataIndex++;
    uart_write_bytes(UART_PORT_NUM,cmdArry,dataIndex); 
}