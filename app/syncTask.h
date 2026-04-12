#ifndef SYNCTASK_H
#define SYNCTASK_H

extern volatile float virtualTime;
extern volatile uint16_t decodePrev;
extern volatile uint16_t decodeNow;
extern volatile unsigned long lastSync;

void syncTime();   // khai báo hàm

#endif /* SYNCTASK_H */