#ifndef HAPTICS_H
#define HAPTICS_H

int haptics_init(void);

int haptics_capture_ok(void);
int haptics_tx_ok(void);
int haptics_result_received(void);

#endif