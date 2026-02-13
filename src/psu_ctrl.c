// Copyright 2023 the X project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "psu_ctrl.h"
#include <events/psuctrl_event.h>
#include <zephyr/random/random.h>
#include "ui.h"

LOG_MODULE_REGISTER(PSUCtrl, LOG_LEVEL_WRN);

#ifndef OVP_SCALE_FACTOR
#define OVP_SCALE_FACTOR 1.0
#endif

static int PSUCtrl_deviceIsReady();
static int PSUCtrl_readEEPROM();
static int PSUCtrl_readVar(uint8_t address, uint8_t* writeInts, uint32_t writeLen, uint8_t* data, uint32_t readCount);
static int PSUCtrl_writeVar(uint8_t address, uint8_t* data, uint32_t len);
static int PSUCtrl_readDPS1200(uint8_t reg, uint8_t* data, uint32_t count);
static int PSUCtrl_readDPS1200Register(uint8_t reg, int *value);
static int PSUCtrl_writeDPS1200(uint8_t reg, int value);
static int PSUCtrl_deinit(void);


static const struct device *i2cdev_;
static int i2cbus_;
static int address_;
static int EEaddress_;
static int numReg_;
static uint16_t* lastReg_;
static uint16_t* minReg_;
static uint16_t* maxReg_;
struct k_mutex mutex_;

int PSUCtrl_init(uint8_t i2cbus, uint8_t address) {
        address_ = 0x58 + address;
        EEaddress_ = 0x50 + address;
        const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
        if (!dev) {
                printf("Failed to get I2C device binding\n");
                return -1;
        }

        i2cdev_ = dev;
        numReg_ = 0x58 / 2;
        lastReg_ = (uint16_t *)k_malloc(numReg_);
        memset(lastReg_, 0, numReg_ * sizeof(uint16_t));
        minReg_ = (uint16_t*) k_malloc(numReg_);
        memset(minReg_, 0xff, numReg_ * sizeof(uint16_t));
        maxReg_ = (uint16_t*)k_malloc(numReg_);
        memset(maxReg_, 0, numReg_ * sizeof(uint16_t));

        k_mutex_init(&mutex_);

        return 0;
}
int PSUCtrl_deviceIsReady() {
        if (!device_is_ready(i2cdev_)) {
                printf("I2C device is not ready\n");
                return 0;
        }
        return 1;
}

int PSUCtrl_deinit() {

        k_free(lastReg_);
        k_free(minReg_);
        k_free(maxReg_);

        return 0;
}

int PSUCtrl_readEEPROM() {
        uint8_t data[256];

        // k_mutex_lock(&mutex_, K_FOREVER);
        if(k_mutex_lock(&mutex_, K_MSEC(100))) {
                printf("i2c mutex lock failed\n");
                return -1;
        }

        if (i2c_burst_read(i2cdev_, EEaddress_, 0, data, 256)) {
                printf("Failed to read EEPROM\n");
                k_mutex_unlock(&mutex_);
                return -2;
        }

        k_mutex_unlock(&mutex_);
        printf("%02x", data[0]);
        for (int i = 1; i < 256; i++) {
                printf(" %02x", data[i]);
        }
        printf("\n");
        return 0;
}

int PSUCtrl_readVar(uint8_t address, uint8_t* writeInts, uint32_t writeLen, uint8_t* data, uint32_t readCount) {
        struct i2c_msg msgs[] = {
                {
                        .buf = writeInts,
                        .len = writeLen,
                        .flags = I2C_MSG_WRITE,
                },
                {
                        .buf = data,
                        .len = readCount,
                        .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP,
                }
        };
        // k_mutex_lock(&mutex_, K_FOREVER);
        if(k_mutex_lock(&mutex_, K_MSEC(100))) {
                printf("i2c mutex lock failed\n");
                return -1;
        }
        // if (i2c_transfer(i2cdev_, &msgs[0], 2, address)) {
        //         printf("Failed to read variable\n");
        //         k_mutex_unlock(&mutex_);
        //         return -1;
        // }

        if(i2c_write(i2cdev_, writeInts, writeLen, address)) {
                printf("Failed to write i2c\n");
                k_mutex_unlock(&mutex_);
                return -1;
        }

        if(i2c_read(i2cdev_, data, readCount, address)) {
                printf("Failed to read i2c\n");
                k_mutex_unlock(&mutex_);
                return -2;
        }

        k_mutex_unlock(&mutex_);

        return 0;
}

int PSUCtrl_writeVar(uint8_t address, uint8_t* data, uint32_t len) {
        uint8_t reg = data[0];
        uint8_t *buf = &data[1];

        // k_mutex_lock(&mutex_, K_FOREVER);
        if(k_mutex_lock(&mutex_, K_MSEC(100))) {
                printf("i2c mutex lock failed\n");
                return -1;
        }

        if (i2c_burst_write(i2cdev_, address, reg, (uint8_t *)buf, len - 1)) {
                k_mutex_unlock(&mutex_);
                return -1;
        }
        k_mutex_unlock(&mutex_);

        return 0;
}

int PSUCtrl_readDPS1200(uint8_t reg, uint8_t* data, uint32_t count) {
        uint8_t cs = reg + (address_ << 1);
        uint8_t regCS = ((0xff - cs) + 1) & 0xff;

        uint8_t writeInts[] = { reg, regCS };
        return PSUCtrl_readVar(address_, writeInts, 2, data, count);
}

int PSUCtrl_readDPS1200Register(uint8_t reg, int *value) {
        uint8_t data[3];
        int ret = PSUCtrl_readDPS1200(reg<<1, data, 3);	 // if low bit set returns zeros (so use even # cmds)
        if(ret < 0) {
                return -1;
        }

        int replyCS = 0;
        for(int i=0; i < sizeof(data); i++) {
                replyCS+=data[i];
        }

        replyCS=((0xff-replyCS)+1)&0xff;	//check reply checksum (not really reqd)
        if(replyCS!=0) {
                printf("Read error");
                return -2;
        }
        *value=data[0] | data[1]<<8;
        return 0;

}

int PSUCtrl_writeDPS1200(uint8_t reg, int value) {
        uint8_t valLSB = value&0xff;
        uint8_t valMSB=value>>8;
        uint8_t cs=(address_<<1)+reg+valLSB+valMSB;
        uint8_t regCS=((0xff-cs)+1)&0xff;
        uint8_t writeInts[] = {reg,valLSB,valMSB,regCS};
        return PSUCtrl_writeVar(address_, writeInts, 4);
}

int PSUCtrl_forceFanRPM(int rpm) {
        PSUCtrl_writeDPS1200(0x40,rpm);
        return 0;
}


void PSUCtrl_ONOFF(lv_event_t * e) {
        lv_obj_t * obj = lv_event_get_target(e);
        lv_event_code_t code = lv_event_get_code(e);
        int val;
        int ret;

        if(code == LV_EVENT_CLICKED) {
                ret = PSUCtrl_readDPS1200Register(0x21, &val);
                if(ret) {
                        return ;
                }
                val ^= 1 << 15;
                PSUCtrl_writeDPS1200(0x42, val);
                ret = PSUCtrl_readDPS1200Register(0x21, &val);
                if(!ret) {
                        if( val & (1 << 15)) {
                                lv_label_set_text(ui_LabelOnOff, "ON");
                                printf("===ON===\n");
                        } else {
                                lv_label_set_text(ui_LabelOnOff, "OFF");
                                printf("===OFF===\n");
                        }
                }
        }
}

// 0 - CV 1 - CC
void PSUCtrl_CVCC(lv_event_t * e) {
        lv_obj_t * obj = lv_event_get_target(e);
        lv_event_code_t code = lv_event_get_code(e);
        int val;
        int ret;

        if(code == LV_EVENT_CLICKED) {
                ret = PSUCtrl_readDPS1200Register(0x21, &val);
                if(ret) {
                        return ;
                }
                val ^= 1;
                PSUCtrl_writeDPS1200(0x42, val);
                ret = PSUCtrl_readDPS1200Register(0x21, &val);
                if(!ret) {
                        if(val & 0x1) {
                                lv_label_set_text(ui_LabelCVCC, "CC");
                                printf("===CC===\n");
                        } else {
                                lv_label_set_text(ui_LabelCVCC, "CV");
                                printf("===CV===\n");
                        }
                }
        }
}

void PSUCtrl_Set_CC(lv_event_t * event) {
        lv_event_code_t code = lv_event_get_code(event);
        int val;
        int ret;
        float current;
        char buf[32];

        ret = PSUCtrl_readDPS1200Register(0x1f, &val);
        if(ret) {
                return ;
        }

        current = val * 1.0/265;

        if (code == LV_EVENT_KEY) {
                uint32_t key = lv_event_get_key(event); // Retrieve the pressed key
                if (key == LV_KEY_UP) {
                        current += 0.1;
                } else if (key == LV_KEY_DOWN) {
                        current -= 0.1;
                }

        } else if(code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
                uint32_t key = lv_event_get_key(event);
                if(key == LV_KEY_UP) current = +5;
                else if(key == LV_KEY_DOWN) current = -5;
        }

        if(current < 5) {
                current = 5;
        }

        if(current > 100) {
                current = 100;
        }

        val = (uint16_t)(current * 265);
        snprintf(buf, sizeof(buf), "%.1f", current);

        PSUCtrl_writeDPS1200(0x3e, val);
        ret = PSUCtrl_readDPS1200Register(0x1f, &val);
        if(!ret) {
        }

        lv_label_set_text(ui_lblPsuCc, buf);

}

int PSUCtrl_UI_Init()
{
        int val;
        int ret;
        float current = 20;
        char buf[32];

        ret = PSUCtrl_readDPS1200Register(0x21, &val);
        if(!ret) {
                if( val & (1 << 15)) {
                        lv_label_set_text(ui_LabelOnOff, "ON");
                } else {
                        lv_label_set_text(ui_LabelOnOff, "OFF");
                }
                if(val & 0x1) {
                        lv_label_set_text(ui_LabelCVCC, "CC");
                } else {
                        lv_label_set_text(ui_LabelCVCC, "CV");
                }
        }

        val = (uint16_t)(current * 265);
        snprintf(buf, sizeof(buf), "%.1f", current);

        PSUCtrl_writeDPS1200(0x3e, val);
        ret = PSUCtrl_readDPS1200Register(0x1f, &val);
        if(!ret) {
                lv_label_set_text(ui_lblPsuCc, buf);
        }

        return 0;
}

ZBUS_CHAN_DEFINE(psuctrl_data_chan,
                 struct psuctrl_data_event,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(watchface_psuctrl_event),
                 ZBUS_MSG_INIT()
                 );

ZBUS_CHAN_DECLARE(psuctrl_data_chan);

#define PSUCTRL_INTERVAL_MS  500

static void psuctrl_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(psuctrl_work, psuctrl_work_cb);

static float volts = 0;
static float amps = 0;
static float watts = 0;
static float energy = 0;
static uint64_t start_time_ms;

static void send_psuctrl_data_event(void)
{
        int val;
        int ret;

        ret=PSUCtrl_readDPS1200Register(7, &val);
        if(!ret) {
                volts = val;
                volts = volts / (OVP_SCALE_FACTOR * 256);
        }
        ret=PSUCtrl_readDPS1200Register(8, &val);
        if(!ret) {
                amps = val;
                amps = amps/64.0;
        }
        ret=PSUCtrl_readDPS1200Register(9, &val);
        if(!ret) {
                watts = val;
        }
        uint64_t elapsed_time = k_uptime_delta(&start_time_ms);
        start_time_ms = k_uptime_get();

        float power = volts * amps;
        energy += power * elapsed_time / 1000.0 / 3600;

        struct psuctrl_data_event evt = {
                .volts = volts,
                .amps = amps,
                .watts = watts,
                .energy = energy,
                .is_kWh = 0
        };

        if(energy >= 1000) {
                evt.energy /= 1000;
                evt.is_kWh = 1;
        } else {
                evt.is_kWh = 0;
        }

        zbus_chan_pub(&psuctrl_data_chan, &evt, K_MSEC(250));
}

static void psuctrl_work_cb(struct k_work *work)
{
        send_psuctrl_data_event();
        k_work_schedule(&psuctrl_work, K_MSEC(PSUCTRL_INTERVAL_MS));
}

int psuctrl_init(void)
{
        start_time_ms = k_uptime_get();
        PSUCtrl_init(0, 7);
        k_work_schedule(&psuctrl_work, K_MSEC(10));
}

// SYS_INIT(psuctrl_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
