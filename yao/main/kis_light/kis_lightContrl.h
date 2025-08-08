#ifndef _KIS_LIGHTCONTRL_H_
#define _KIS_LIGHTCONTRL_H_
#ifdef __cplusplus
extern "C" {
#endif

void uart1Initr(void);
void setLevelCallback(unsigned char levelValue,unsigned short lightEndpointId);
unsigned char crc8Calculate_receive(unsigned short length, unsigned char *data);
void sendDataToZigbee(unsigned char *dataAddr,unsigned char dataLen,unsigned short cluseCmd,unsigned short deviceEndpointId);
#ifdef __cplusplus
}
#endif

#endif//_KIS_LIGHTCONTRL_H_