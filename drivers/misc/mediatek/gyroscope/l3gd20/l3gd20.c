/*
* Copyright (C) 2011-2014 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/


#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>

#include <cust_gyro.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include "l3gd20.h"
#include <linux/hwmsen_helper.h>
#include <linux/kernel.h>

#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>
#include <mach/mt_boot.h>
#include <gyroscope.h>
#include <linux/batch.h>
#include <mach/sensors_ssb.h>

#define POWER_NONE_MACRO MT65XX_POWER_NONE


/*----------------------------------------------------------------------------*/
#define I2C_DRIVERID_L3GD20    3000

/*---------------------------------------------------------------------------*/
#define DEBUG 1
/*----------------------------------------------------------------------------*/
#define CONFIG_L3GD20_LOWPASS   /*apply low pass filter on output*/
/*----------------------------------------------------------------------------*/
#define L3GD20_AXIS_X          0
#define L3GD20_AXIS_Y          1
#define L3GD20_AXIS_Z          2
#define L3GD20_AXES_NUM        3
#define L3GD20_DATA_LEN        6
#define L3GD20_DEV_NAME        "L3GD20"
/*----------------------------------------------------------------------------*/
static const struct i2c_device_id l3gd20_i2c_id[] = {{L3GD20_DEV_NAME,0},{}};


/*----------------------------------------------------------------------------*/
static int l3gd20_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int l3gd20_i2c_remove(struct i2c_client *client);
static int l3gd20_init_client(struct i2c_client *client, bool enable);

extern struct gyro_hw* l3gd20_get_cust_gyro_hw(void);
static int l3gd20_local_init(void);
static int  l3gd20_remove(void);
static int l3gd20_init_flag =-1;
static struct gyro_init_info l3gd20_init_info = {
        .name = "l3gd20",
        .init = l3gd20_local_init,
        .uninit =l3gd20_remove,
};

/*----------------------------------------------------------------------------*/
typedef enum {
    GYRO_TRC_FILTER  = 0x01,
    GYRO_TRC_RAWDATA = 0x02,
    GYRO_TRC_IOCTL   = 0x04,
    GYRO_TRC_CALI    = 0X08,
    GYRO_TRC_INFO    = 0X10,
    GYRO_TRC_DATA    = 0X20,
} GYRO_TRC;
/*----------------------------------------------------------------------------*/
struct scale_factor{
    u8  whole;
    u8  fraction;
};
/*----------------------------------------------------------------------------*/
struct data_resolution {
    struct scale_factor scalefactor;
    int                 sensitivity;
};
/*----------------------------------------------------------------------------*/
#define C_MAX_FIR_LENGTH (32)
/*----------------------------------------------------------------------------*/
struct data_filter {
    s16 raw[C_MAX_FIR_LENGTH][L3GD20_AXES_NUM];
    int sum[L3GD20_AXES_NUM];
    int num;
    int idx;
};
/*----------------------------------------------------------------------------*/
struct l3gd20_i2c_data {
    struct i2c_client *client;
    struct gyro_hw *hw;
    struct hwmsen_convert   cvt;

    /*misc*/
    struct data_resolution *reso;
    atomic_t                trace;
    atomic_t                suspend;
    atomic_t                selftest;
    atomic_t                filter;
    s16                     cali_sw[L3GD20_AXES_NUM+1];

    /*data*/
    s8                      offset[L3GD20_AXES_NUM+1];  /*+1: for 4-byte alignment*/
    s16                     data[L3GD20_AXES_NUM+1];

#if defined(CONFIG_L3GD20_LOWPASS)
    atomic_t                firlen;
    atomic_t                fir_en;
    struct data_filter      fir;
#endif
    /*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND)
    struct early_suspend    early_drv;
#endif
};
/*----------------------------------------------------------------------------*/
static struct i2c_driver l3gd20_i2c_driver = {
    .driver = {
//      .owner          = THIS_MODULE,
        .name           = L3GD20_DEV_NAME,
    },
    .probe              = l3gd20_i2c_probe,
    .remove                = l3gd20_i2c_remove,
    //.detect                = l3gd20_i2c_detect,
#if !defined(CONFIG_HAS_EARLYSUSPEND)
    .suspend            = l3gd20_suspend,
    .resume             = l3gd20_resume,
#endif
    .id_table = l3gd20_i2c_id,
//    .address_data = &l3gd20_addr_data,
};

/*----------------------------------------------------------------------------*/
static struct i2c_client *l3gd20_i2c_client = NULL;
//static struct platform_driver l3gd20_gyro_driver;
static struct l3gd20_i2c_data *obj_i2c_data = NULL;
static bool sensor_power = false;



/*----------------------------------------------------------------------------*/
//#define GYRO_TAG                  "[Gyroscope] "
//#define GYRO_FUN(f)               printk(KERN_INFO GYRO_TAG"%s\n", __FUNCTION__)
//#define GYRO_ERR(fmt, args...)    printk(KERN_ERR GYRO_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args)

//#define GYRO_LOG(fmt, args...)    printk(KERN_INFO GYRO_TAG fmt, ##args)

//#define GYRO_FUN(f)               printk(GYRO_TAG"%s\n", __FUNCTION__)
//#define GYRO_ERR(fmt, args...)    printk(KERN_ERR GYRO_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args)
//#define GYRO_LOG(fmt, args...)    printk(GYRO_TAG fmt, ##args)

/*----------------------------------------------------------------------------*/

static void L3GD20_dumpReg(struct i2c_client *client)
{
  int i=0;
  u8 addr = 0x20;
  u8 regdata=0;
  for(i=0; i<25 ; i++)
  {
    //dump all
    hwmsen_read_byte(client,addr,&regdata);
    HWM_LOG("Reg addr=%x regdata=%x\n",addr,regdata);
    addr++;

  }

}


/*--------------------gyroscopy power control function----------------------------------*/
static void L3GD20_power(struct gyro_hw *hw, unsigned int on)
{
    static unsigned int power_on = 0;

    if(hw->power_id != POWER_NONE_MACRO)        // have externel LDO
    {
        GYRO_LOG("power %s\n", on ? "on" : "off");
        if(power_on == on)    // power status not change
        {
            GYRO_LOG("ignore power control: %d\n", on);
        }
        else if(on)    // power on
        {
            if(!hwPowerOn(hw->power_id, hw->power_vol, "L3GD20"))
            {
                GYRO_ERR("power on fails!!\n");
            }
        }
        else    // power off
        {
            if (!hwPowerDown(hw->power_id, "L3GD20"))
            {
                GYRO_ERR("power off fail!!\n");
            }
        }
    }
    power_on = on;
}
/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
static int L3GD20_write_rel_calibration(struct l3gd20_i2c_data *obj, int dat[L3GD20_AXES_NUM])
{
    obj->cali_sw[L3GD20_AXIS_X] = obj->cvt.sign[L3GD20_AXIS_X]*dat[obj->cvt.map[L3GD20_AXIS_X]];
    obj->cali_sw[L3GD20_AXIS_Y] = obj->cvt.sign[L3GD20_AXIS_Y]*dat[obj->cvt.map[L3GD20_AXIS_Y]];
    obj->cali_sw[L3GD20_AXIS_Z] = obj->cvt.sign[L3GD20_AXIS_Z]*dat[obj->cvt.map[L3GD20_AXIS_Z]];
#if DEBUG
        if(atomic_read(&obj->trace) & GYRO_TRC_CALI)
        {
            GYRO_LOG("test  (%5d, %5d, %5d) ->(%5d, %5d, %5d)->(%5d, %5d, %5d))\n",
                obj->cvt.sign[L3GD20_AXIS_X],obj->cvt.sign[L3GD20_AXIS_Y],obj->cvt.sign[L3GD20_AXIS_Z],
                dat[L3GD20_AXIS_X], dat[L3GD20_AXIS_Y], dat[L3GD20_AXIS_Z],
                obj->cvt.map[L3GD20_AXIS_X],obj->cvt.map[L3GD20_AXIS_Y],obj->cvt.map[L3GD20_AXIS_Z]);
            GYRO_LOG("write gyro calibration data  (%5d, %5d, %5d)\n",
                obj->cali_sw[L3GD20_AXIS_X],obj->cali_sw[L3GD20_AXIS_Y],obj->cali_sw[L3GD20_AXIS_Z]);
        }
#endif
    return 0;
}


/*----------------------------------------------------------------------------*/
static int L3GD20_ResetCalibration(struct i2c_client *client)
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);

    memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
    return 0;
}
/*----------------------------------------------------------------------------*/
static int L3GD20_ReadCalibration(struct i2c_client *client, int dat[L3GD20_AXES_NUM])
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);

    dat[obj->cvt.map[L3GD20_AXIS_X]] = obj->cvt.sign[L3GD20_AXIS_X]*obj->cali_sw[L3GD20_AXIS_X];
    dat[obj->cvt.map[L3GD20_AXIS_Y]] = obj->cvt.sign[L3GD20_AXIS_Y]*obj->cali_sw[L3GD20_AXIS_Y];
    dat[obj->cvt.map[L3GD20_AXIS_Z]] = obj->cvt.sign[L3GD20_AXIS_Z]*obj->cali_sw[L3GD20_AXIS_Z];

#if DEBUG
        if(atomic_read(&obj->trace) & GYRO_TRC_CALI)
        {
            GYRO_LOG("Read gyro calibration data  (%5d, %5d, %5d)\n",
                dat[L3GD20_AXIS_X],dat[L3GD20_AXIS_Y],dat[L3GD20_AXIS_Z]);
        }
#endif
          GYRO_LOG("Read gyro calibration data  (%5d, %5d, %5d)\n",
                dat[L3GD20_AXIS_X],dat[L3GD20_AXIS_Y],dat[L3GD20_AXIS_Z]);
    return 0;
}
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int L3GD20_WriteCalibration(struct i2c_client *client, int dat[L3GD20_AXES_NUM])
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);
    int err = 0;
    int cali[L3GD20_AXES_NUM];


    GYRO_FUN();
    if(!obj || ! dat)
    {
        GYRO_ERR("null ptr!!\n");
        return -EINVAL;
    }
    else
    {
        cali[obj->cvt.map[L3GD20_AXIS_X]] = obj->cvt.sign[L3GD20_AXIS_X]*obj->cali_sw[L3GD20_AXIS_X];
        cali[obj->cvt.map[L3GD20_AXIS_Y]] = obj->cvt.sign[L3GD20_AXIS_Y]*obj->cali_sw[L3GD20_AXIS_Y];
        cali[obj->cvt.map[L3GD20_AXIS_Z]] = obj->cvt.sign[L3GD20_AXIS_Z]*obj->cali_sw[L3GD20_AXIS_Z];
        cali[L3GD20_AXIS_X] += dat[L3GD20_AXIS_X];
        cali[L3GD20_AXIS_Y] += dat[L3GD20_AXIS_Y];
        cali[L3GD20_AXIS_Z] += dat[L3GD20_AXIS_Z];
#if DEBUG
        if(atomic_read(&obj->trace) & GYRO_TRC_CALI)
        {
            GYRO_LOG("write gyro calibration data  (%5d, %5d, %5d)-->(%5d, %5d, %5d)\n",
                dat[L3GD20_AXIS_X], dat[L3GD20_AXIS_Y], dat[L3GD20_AXIS_Z],
                cali[L3GD20_AXIS_X],cali[L3GD20_AXIS_Y],cali[L3GD20_AXIS_Z]);
        }
#endif
        return L3GD20_write_rel_calibration(obj, cali);
    }

    return err;
}
/*----------------------------------------------------------------------------*/
static int L3GD20_CheckDeviceID(struct i2c_client *client)
{
    u8 databuf[10];
    int res = 0;

    memset(databuf, 0, sizeof(u8)*10);
    databuf[0] = L3GD20_FIXED_DEVID;

    res = hwmsen_read_byte(client,L3GD20_REG_DEVID,databuf);
    GYRO_LOG(" L3GD20  id %x!\n",databuf[0]);
    if(databuf[0]!=L3GD20_FIXED_DEVID)
    {
        return L3GD20_ERR_IDENTIFICATION;
    }

    //exit_MMA8453Q_CheckDeviceID:
    if (res < 0)
    {
        return L3GD20_ERR_I2C;
    }

    return L3GD20_SUCCESS;
}


//----------------------------------------------------------------------------//
static int L3GD20_SetPowerMode(struct i2c_client *client, bool enable)
{
    u8 databuf[2] = {0};
    int res = 0;

    if(enable == sensor_power)
    {
        GYRO_LOG("Sensor power status is newest!\n");
        return L3GD20_SUCCESS;
    }

    if(hwmsen_read_byte(client, L3GD20_CTL_REG1, databuf))
    {
        GYRO_ERR("read power ctl register err!\n");
        return L3GD20_ERR_I2C;
    }

    databuf[0] &= ~L3GD20_POWER_ON;//clear power on bit
    if(true == enable )
    {
        databuf[0] |= L3GD20_POWER_ON;
    }
    else
    {
        // do nothing
    }
    databuf[1] = databuf[0];
    databuf[0] = L3GD20_CTL_REG1;
    res = i2c_master_send(client, databuf, 0x2);
    if(res <= 0)
    {
        GYRO_LOG("set power mode failed!\n");
        return L3GD20_ERR_I2C;
    }
    else
    {
        GYRO_LOG("set power mode ok %d!\n", enable);
    }

    sensor_power = enable;

    return L3GD20_SUCCESS;
}

/*----------------------------------------------------------------------------*/


static int L3GD20_SetDataResolution(struct i2c_client *client, u8 dataResolution)
{
    u8 databuf[2] = {0};
    int res = 0;
    GYRO_FUN();

    if(hwmsen_read_byte(client, L3GD20_CTL_REG4, databuf))
    {
        GYRO_ERR("read L3GD20_CTL_REG4 err!\n");
        return L3GD20_ERR_I2C;
    }
    else
    {
        GYRO_LOG("read  L3GD20_CTL_REG4 register: 0x%x\n", databuf[0]);
    }

    databuf[0] &= 0xcf;//clear
    databuf[0] |= dataResolution;

    databuf[1] = databuf[0];
    databuf[0] = L3GD20_CTL_REG4;


    res = i2c_master_send(client, databuf, 0x2);
    if(res <= 0)
    {
        GYRO_ERR("write SetDataResolution register err!\n");
        return L3GD20_ERR_I2C;
    }
    return L3GD20_SUCCESS;
}

// set the sample rate
static int L3GD20_SetSampleRate(struct i2c_client *client, u8 sample_rate)
{
    u8 databuf[2] = {0};
    int res = 0;
    GYRO_FUN();

    if(hwmsen_read_byte(client, L3GD20_CTL_REG1, databuf))
    {
        GYRO_ERR("read gyro data format register err!\n");
        return L3GD20_ERR_I2C;
    }
    else
    {
        GYRO_LOG("read  gyro data format register: 0x%x\n", databuf[0]);
    }

    databuf[0] &= 0x3f;//clear
    databuf[0] |= sample_rate;

    databuf[1] = databuf[0];
    databuf[0] = L3GD20_CTL_REG1;


    res = i2c_master_send(client, databuf, 0x2);
    if(res <= 0)
    {
        GYRO_ERR("write sample rate register err!\n");
        return L3GD20_ERR_I2C;
    }

    return L3GD20_SUCCESS;
}
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
static int L3GD20_ReadGyroData(struct i2c_client *client, char *buf, int bufsize)
{
    char databuf[6];
    int data[3];
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);

    if(sensor_power == false)
    {
        L3GD20_SetPowerMode(client, true);
    }

    if(hwmsen_read_block(client, AUTO_INCREMENT |L3GD20_REG_GYRO_XL, databuf, 6))
    {
        GYRO_ERR("L3GD20 read gyroscope data  error\n");
        return -2;
    }
    else
    {
        obj->data[L3GD20_AXIS_X] = (s16)((databuf[L3GD20_AXIS_X*2+1] << 8) | (databuf[L3GD20_AXIS_X*2]));
        obj->data[L3GD20_AXIS_Y] = (s16)((databuf[L3GD20_AXIS_Y*2+1] << 8) | (databuf[L3GD20_AXIS_Y*2]));
        obj->data[L3GD20_AXIS_Z] = (s16)((databuf[L3GD20_AXIS_Z*2+1] << 8) | (databuf[L3GD20_AXIS_Z*2]));

#if DEBUG
        if(atomic_read(&obj->trace) & GYRO_TRC_RAWDATA)
        {
            GYRO_LOG("read gyro register: %x, %x, %x, %x, %x, %x",
                databuf[0], databuf[1], databuf[2], databuf[3], databuf[4], databuf[5]);
            GYRO_LOG("get gyro raw data (0x%08X, 0x%08X, 0x%08X) -> (%5d, %5d, %5d)\n",
                obj->data[L3GD20_AXIS_X],obj->data[L3GD20_AXIS_Y],obj->data[L3GD20_AXIS_Z],
                obj->data[L3GD20_AXIS_X],obj->data[L3GD20_AXIS_Y],obj->data[L3GD20_AXIS_Z]);
        }
#endif

        obj->data[L3GD20_AXIS_X] = obj->data[L3GD20_AXIS_X] + obj->cali_sw[L3GD20_AXIS_X];
        obj->data[L3GD20_AXIS_Y] = obj->data[L3GD20_AXIS_Y] + obj->cali_sw[L3GD20_AXIS_Y];
        obj->data[L3GD20_AXIS_Z] = obj->data[L3GD20_AXIS_Z] + obj->cali_sw[L3GD20_AXIS_Z];

        /*remap coordinate*/
        data[obj->cvt.map[L3GD20_AXIS_X]] = obj->cvt.sign[L3GD20_AXIS_X]*obj->data[L3GD20_AXIS_X];
        data[obj->cvt.map[L3GD20_AXIS_Y]] = obj->cvt.sign[L3GD20_AXIS_Y]*obj->data[L3GD20_AXIS_Y];
        data[obj->cvt.map[L3GD20_AXIS_Z]] = obj->cvt.sign[L3GD20_AXIS_Z]*obj->data[L3GD20_AXIS_Z];


        //Out put the degree/second(o/s)
        data[L3GD20_AXIS_X] = data[L3GD20_AXIS_X] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;
        data[L3GD20_AXIS_Y] = data[L3GD20_AXIS_Y] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;
        data[L3GD20_AXIS_Z] = data[L3GD20_AXIS_Z] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;

    }

    sprintf(buf, "%04x %04x %04x", data[L3GD20_AXIS_X],data[L3GD20_AXIS_Y],data[L3GD20_AXIS_Z]);

#if DEBUG
    if(atomic_read(&obj->trace) & GYRO_TRC_DATA)
    {
        GYRO_LOG("get gyro data packet:[%d %d %d]\n", data[0], data[1], data[2]);
    }
#endif

    return 0;

}

// if we use internel fifo then compile the function L3GD20_SET_FIFO_MODE
#if 0

/*----------------------------------------------------------------------------*/
static int L3GD20_SET_FIFO_MODE(struct i2c_client *client,u8 config)
{
    u8 databuf[2] = {0};
    int res = 0;
    GYRO_FUN();

    if(hwmsen_read_byte(client, L3GD20_FIFO_CTL, databuf))
    {
        GYRO_ERR("read L3GD20_CTL_REG4 err!\n");
        return L3GD20_ERR_I2C;
    }
    else
    {
        GYRO_LOG("read  L3GD20_CTL_REG4 register: 0x%x\n", databuf[0]);
    }

    databuf[0] &= 0x1f;//clear
    databuf[0] |= config;

    databuf[1] = databuf[0];
    databuf[0] = L3GD20_FIFO_CTL;


    res = i2c_master_send(client, databuf, 0x2);
    if(res <= 0)
    {
        GYRO_ERR("write L3GD20_SET_FIFO_MODE register err!\n");
        return L3GD20_ERR_I2C;
    }
    return L3GD20_SUCCESS;
}
#endif

static int L3GD20_SelfTest(struct i2c_client *client)
{
    int err =0;
    u8 data=0;
    char strbuf[L3GD20_BUFSIZE] = {0};
    int avgx_NOST,avgy_NOST,avgz_NOST;
    int sumx,sumy,sumz;
    int avgx_ST,avgy_ST,avgz_ST;
    int nost_x,nost_y,nost_z=0;
    int st_x,st_y,st_z=0;

    int resx,resy,resz=-1;
    int i=0;
    int testRes=0;
    int sampleNum =5;

    sumx=sumy=sumz=0;
    // 1 init
    err = l3gd20_init_client(client, true);
    if(err)
    {
        GYRO_ERR("initialize client fail! err code %d!\n", err);
        return - 2;
    }
    L3GD20_dumpReg(client);
    // 2 check ZYXDA bit
    hwmsen_read_byte(client,L3GD20_STATUS_REG,&data);
    GYRO_LOG("L3GD20_STATUS_REG=%d\n",data );
    while(0x04 != (data&0x04))
    {
      msleep(10);
    }
    msleep(1000); //wait for stable
    // 3 read raw data no self test data
    for(i=0; i<sampleNum; i++)
    {
      L3GD20_ReadGyroData(client, strbuf, L3GD20_BUFSIZE);
      sscanf(strbuf, "%x %x %x", &nost_x, &nost_y, &nost_z);
      GYRO_LOG("NOst %d %d %d!\n", nost_x,nost_y,nost_z);
      sumx += nost_x;
      sumy += nost_y;
      sumz += nost_z;
      msleep(10);
    }
    //calculate avg x y z
    avgx_NOST = sumx/sampleNum;
    avgy_NOST = sumy/sampleNum;
    avgz_NOST = sumz/sampleNum;
    GYRO_LOG("avg NOST %d %d %d!\n", avgx_NOST,avgy_NOST,avgz_NOST);

    // 4 enalbe selftest
    hwmsen_read_byte(client,L3GD20_CTL_REG4,&data);
    data = data | 0x02;
    hwmsen_write_byte(client,L3GD20_CTL_REG4,data);

    msleep(1000);//wait for stable

    L3GD20_dumpReg(client);
    // 5 check  ZYXDA bit

    //6 read raw data   self test data
    sumx=0;
    sumy=0;
    sumz=0;
    for(i=0; i<sampleNum; i++)
    {
      L3GD20_ReadGyroData(client, strbuf, L3GD20_BUFSIZE);
      sscanf(strbuf, "%x %x %x", &st_x, &st_y, &st_z);
      GYRO_LOG("st %d %d %d!\n", st_x,st_y,st_z);

      sumx += st_x;
      sumy += st_y;
      sumz += st_z;

      msleep(10);
    }
    // 7 calc calculate avg x y z ST
    avgx_ST = sumx/sampleNum;
    avgy_ST = sumy/sampleNum;
    avgz_ST = sumz/sampleNum;
    //GYRO_LOG("avg ST %d %d %d!\n", avgx_ST,avgy_ST,avgz_ST);
    //GYRO_LOG("abs(avgx_ST-avgx_NOST): %ld \n", abs(avgx_ST-avgx_NOST));
    //GYRO_LOG("abs(avgy_ST-avgy_NOST): %ld \n", abs(avgy_ST-avgy_NOST));
    //GYRO_LOG("abs(avgz_ST-avgz_NOST): %ld \n", abs(avgz_ST-avgz_NOST));

    if((abs(avgx_ST-avgx_NOST)>=175*131) && (abs(avgx_ST-avgx_NOST)<=875*131))
    {
      resx =0; //x axis pass
      GYRO_LOG(" x axis pass\n" );
    }
    if((abs(avgy_ST-avgy_NOST)>=175*131) && (abs(avgy_ST-avgy_NOST)<=875*131))
    {
      resy =0; //y axis pass
      GYRO_LOG(" y axis pass\n" );
    }
    if((abs(avgz_ST-avgz_NOST)>=175*131) && (abs(avgz_ST-avgz_NOST)<=875*131))
    {
      resz =0; //z axis pass
      GYRO_LOG(" z axis pass\n" );
    }

    if(0==resx && 0==resy && 0==resz)
    {
      testRes = 0;
    }
    else
    {
      testRes = -1;
    }

    hwmsen_write_byte(client,L3GD20_CTL_REG4,0x00);
    err = l3gd20_init_client(client, false);
    if(err)
    {
        GYRO_ERR("initialize client fail! err code %d!\n", err);
        return -2;
    }
    GYRO_LOG("testRes %d!\n", testRes);
    return testRes;

}


//self test for factory
static int L3GD20_SMTReadSensorData(struct i2c_client *client)
{
    //S16 gyro[L3GD20_AXES_NUM*L3GD20_FIFOSIZE];
    int res = 0;

    GYRO_FUN();
    res = L3GD20_SelfTest(client);

    GYRO_LOG(" L3GD20_SMTReadSensorData %d", res );

    return res;
}

/*----------------------------------------------------------------------------*/
static int L3GD20_ReadChipInfo(struct i2c_client *client, char *buf, int bufsize)
{
    u8 databuf[10];

    memset(databuf, 0, sizeof(u8)*10);

    if((NULL == buf)||(bufsize<=30))
    {
        return -1;
    }

    if(NULL == client)
    {
        *buf = 0;
        return -2;
    }

    sprintf(buf, "L3GD20 Chip");
    return 0;
}


/*----------------------------------------------------------------------------*/
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
    struct i2c_client *client = l3gd20_i2c_client;
    char strbuf[L3GD20_BUFSIZE];
    if(NULL == client)
    {
        GYRO_ERR("i2c client is null!!\n");
        return 0;
    }

    L3GD20_ReadChipInfo(client, strbuf, L3GD20_BUFSIZE);
    return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}
/*----------------------------------------------------------------------------*/
static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
    struct i2c_client *client = l3gd20_i2c_client;
    char strbuf[L3GD20_BUFSIZE];

    if(NULL == client)
    {
        GYRO_ERR("i2c client is null!!\n");
        return 0;
    }

    L3GD20_ReadGyroData(client, strbuf, L3GD20_BUFSIZE);
    return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);;
}

/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
    ssize_t res;
    struct l3gd20_i2c_data *obj = obj_i2c_data;
    if (obj == NULL)
    {
        GYRO_ERR("i2c_data obj is null!!\n");
        return 0;
    }

    res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));
    return res;
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
    struct l3gd20_i2c_data *obj = obj_i2c_data;
    int trace;
    if (obj == NULL)
    {
        GYRO_ERR("i2c_data obj is null!!\n");
        return 0;
    }

    if(1 == sscanf(buf, "0x%x", &trace))
    {
        atomic_set(&obj->trace, trace);
    }
    else
    {
        GYRO_ERR("invalid content: '%s', length = %d\n", buf, count);
    }

    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_status_value(struct device_driver *ddri, char *buf)
{
    ssize_t len = 0;
    struct l3gd20_i2c_data *obj = obj_i2c_data;
    if (obj == NULL)
    {
        GYRO_ERR("i2c_data obj is null!!\n");
        return 0;
    }

    if(obj->hw)
    {
        len += snprintf(buf+len, PAGE_SIZE-len, "CUST: %d %d (%d %d)\n",
                obj->hw->i2c_num, obj->hw->direction, obj->hw->power_id, obj->hw->power_vol);
    }
    else
    {
        len += snprintf(buf+len, PAGE_SIZE-len, "CUST: NULL\n");
    }
    return len;
}
/*----------------------------------------------------------------------------*/

static DRIVER_ATTR(chipinfo,             S_IRUGO, show_chipinfo_value,      NULL);
static DRIVER_ATTR(sensordata,           S_IRUGO, show_sensordata_value,    NULL);
static DRIVER_ATTR(trace,      S_IWUSR | S_IWGRP | S_IRUGO, show_trace_value,         store_trace_value);
static DRIVER_ATTR(status,               S_IRUGO, show_status_value,        NULL);
/*----------------------------------------------------------------------------*/
static struct driver_attribute *L3GD20_attr_list[] = {
    &driver_attr_chipinfo,     /*chip information*/
    &driver_attr_sensordata,   /*dump sensor data*/
    &driver_attr_trace,        /*trace log*/
    &driver_attr_status,
};
/*----------------------------------------------------------------------------*/
static int l3gd20_create_attr(struct device_driver *driver)
{
    int idx, err = 0;
    int num = (int)(sizeof(L3GD20_attr_list)/sizeof(L3GD20_attr_list[0]));
    if (driver == NULL)
    {
        return -EINVAL;
    }

    for(idx = 0; idx < num; idx++)
    {
        if(0 != (err = driver_create_file(driver, L3GD20_attr_list[idx])))
        {
            GYRO_ERR("driver_create_file (%s) = %d\n", L3GD20_attr_list[idx]->attr.name, err);
            break;
        }
    }
    return err;
}
/*----------------------------------------------------------------------------*/
static int l3gd20_delete_attr(struct device_driver *driver)
{
    int idx ,err = 0;
    int num = (int)(sizeof(L3GD20_attr_list)/sizeof(L3GD20_attr_list[0]));

    if(driver == NULL)
    {
        return -EINVAL;
    }


    for(idx = 0; idx < num; idx++)
    {
        driver_remove_file(driver, L3GD20_attr_list[idx]);
    }


    return err;
}

/*----------------------------------------------------------------------------*/
static int l3gd20_init_client(struct i2c_client *client, bool enable)
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);
    int res = 0;
    GYRO_FUN();
        GYRO_LOG(" fwq l3gd20 addr %x!\n",client->addr);
    res = L3GD20_CheckDeviceID(client);
    GYRO_LOG("L3GD20_CheckDeviceID res = %x\n", res);

    if(res != L3GD20_SUCCESS)
    {
        return res;
    }

    res = L3GD20_SetPowerMode(client, enable);
    GYRO_LOG("L3GD20_SetPowerMode res = %x\n", res);

    if(res != L3GD20_SUCCESS)
    {
        return res;
    }

    // The range should at least be 17.45 rad/s (ie: ~1000 deg/s).
    res = L3GD20_SetDataResolution(client,L3GD20_RANGE_2000);//we have only this choice
    GYRO_LOG("L3GD20_SetDataResolution res = %x\n", res);

    if(res != L3GD20_SUCCESS)
    {
        return res;
    }

    //
    res = L3GD20_SetSampleRate(client, L3GD20_100HZ);
    GYRO_LOG("L3GD20_SetSampleRate res = %x\n", res);

    if(res != L3GD20_SUCCESS )
    {
        return res;
    }

    GYRO_LOG("l3gd20_init_client OK!\n");

#ifdef CONFIG_L3GD20_LOWPASS
    memset(&obj->fir, 0x00, sizeof(obj->fir));
#endif

    return L3GD20_SUCCESS;
}

/*----------------------------------------------------------------------------*/
int l3gd20_operate(void* self, uint32_t command, void* buff_in, int size_in,
        void* buff_out, int size_out, int* actualout)
{
    int err = 0;
    int value;
    struct l3gd20_i2c_data *priv = (struct l3gd20_i2c_data*)self;
    hwm_sensor_data* gyro_data;
    char buff[L3GD20_BUFSIZE];

    switch (command)
    {
        case SENSOR_DELAY:
            if((buff_in == NULL) || (size_in < sizeof(int)))
            {
                GYRO_ERR("Set delay parameter error!\n");
                err = -EINVAL;
            }
            else
            {

            }
            break;

        case SENSOR_ENABLE:
            if((buff_in == NULL) || (size_in < sizeof(int)))
            {
                GYRO_ERR("Enable gyroscope parameter error!\n");
                err = -EINVAL;
            }
            else
            {
                value = *(int *)buff_in;
                if(((value == 0) && (sensor_power == false)) ||((value == 1) && (sensor_power == true)))
                {
                    GYRO_LOG("gyroscope device have updated!\n");
                }
                else
                {
                    err = L3GD20_SetPowerMode(priv->client, !sensor_power);
                }
            }
            break;

        case SENSOR_GET_DATA:
            if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
            {
                GYRO_ERR("get gyroscope data parameter error!\n");
                err = -EINVAL;
            }
            else
            {
                gyro_data = (hwm_sensor_data *)buff_out;
                L3GD20_ReadGyroData(priv->client, buff, L3GD20_BUFSIZE);
                sscanf(buff, "%x %x %x", &gyro_data->values[0],
                                    &gyro_data->values[1], &gyro_data->values[2]);
                gyro_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;
                gyro_data->value_divide = DEGREE_TO_RAD;
            }
            break;
        default:
            GYRO_ERR("gyroscope operate function no this parameter %d!\n", command);
            err = -1;
            break;
    }

    return err;
}

/******************************************************************************
 * Function Configuration
******************************************************************************/
static int l3gd20_open(struct inode *inode, struct file *file)
{
    file->private_data = l3gd20_i2c_client;

    if(file->private_data == NULL)
    {
        GYRO_ERR("null pointer!!\n");
        return -EINVAL;
    }
    return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int l3gd20_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}
/*----------------------------------------------------------------------------*/
//static int l3gd20_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
//       unsigned long arg)
static long l3gd20_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)
{
    struct i2c_client *client = (struct i2c_client*)file->private_data;
    //struct l3gd20_i2c_data *obj = (struct l3gd20_i2c_data*)i2c_get_clientdata(client);
    char strbuf[L3GD20_BUFSIZE] = {0};
    void __user *data;
    long err = 0;
    int copy_cnt = 0;
    SENSOR_DATA sensor_data;
    int cali[3];
    int smtRes=0;
    //GYRO_FUN();

    if(_IOC_DIR(cmd) & _IOC_READ)
    {
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    }
    else if(_IOC_DIR(cmd) & _IOC_WRITE)
    {
        err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    }

    if(err)
    {
        GYRO_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
        return -EFAULT;
    }

    switch(cmd)
    {
        case GYROSCOPE_IOCTL_INIT:
            l3gd20_init_client(client, false);
            break;

        case GYROSCOPE_IOCTL_SMT_DATA:
            data = (void __user *) arg;
            if(data == NULL)
            {
                err = -EINVAL;
                break;
            }

            smtRes = L3GD20_SMTReadSensorData(client);
            GYRO_LOG("IOCTL smtRes: %d!\n", smtRes);
            copy_cnt = copy_to_user(data, &smtRes,  sizeof(smtRes));

            if(copy_cnt)
            {
                err = -EFAULT;
                GYRO_ERR("copy gyro data to user failed!\n");
            }
            GYRO_LOG("copy gyro data to user OK: %d!\n", copy_cnt);
            break;


        case GYROSCOPE_IOCTL_READ_SENSORDATA:
            data = (void __user *) arg;
            if(data == NULL)
            {
                err = -EINVAL;
                break;
            }

            L3GD20_ReadGyroData(client, strbuf, L3GD20_BUFSIZE);
            if(copy_to_user(data, strbuf, sizeof(strbuf)))
            {
                err = -EFAULT;
                break;
            }
            break;

        case GYROSCOPE_IOCTL_SET_CALI:
            data = (void __user*)arg;
            if(data == NULL)
            {
                err = -EINVAL;
                break;
            }
            if(copy_from_user(&sensor_data, data, sizeof(sensor_data)))
            {
                err = -EFAULT;
                break;
            }

            else
            {
                cali[L3GD20_AXIS_X] = sensor_data.x * L3GD20_FS_2000_LSB / L3GD20_OUT_MAGNIFY;
                cali[L3GD20_AXIS_Y] = sensor_data.y * L3GD20_FS_2000_LSB / L3GD20_OUT_MAGNIFY;
                cali[L3GD20_AXIS_Z] = sensor_data.z * L3GD20_FS_2000_LSB / L3GD20_OUT_MAGNIFY;
                err = L3GD20_WriteCalibration(client, cali);
            }
            break;

        case GYROSCOPE_IOCTL_CLR_CALI:
            err = L3GD20_ResetCalibration(client);
            break;

        case GYROSCOPE_IOCTL_GET_CALI:
            data = (void __user*)arg;
            if(data == NULL)
            {
                err = -EINVAL;
                break;
            }
            err = L3GD20_ReadCalibration(client, cali);
            if(err)
            {
                break;
            }

            sensor_data.x = cali[L3GD20_AXIS_X] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;
            sensor_data.y = cali[L3GD20_AXIS_Y] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;
            sensor_data.z = cali[L3GD20_AXIS_Z] * L3GD20_OUT_MAGNIFY / L3GD20_FS_2000_LSB;
            if(copy_to_user(data, &sensor_data, sizeof(sensor_data)))
            {
                err = -EFAULT;
                break;
            }
            break;

        default:
            GYRO_ERR("unknown IOCTL: 0x%08x\n", cmd);
            err = -ENOIOCTLCMD;
            break;
    }
    return err;
}


/*----------------------------------------------------------------------------*/
static struct file_operations l3gd20_fops = {
//    .owner = THIS_MODULE,
    .open = l3gd20_open,
    .release = l3gd20_release,
    .unlocked_ioctl = l3gd20_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice l3gd20_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "gyroscope",
    .fops = &l3gd20_fops,
};
/*----------------------------------------------------------------------------*/
#ifndef CONFIG_HAS_EARLYSUSPEND
/*----------------------------------------------------------------------------*/
static int l3gd20_suspend(struct i2c_client *client, pm_message_t msg)
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);
    GYRO_FUN();

    if(msg.event == PM_EVENT_SUSPEND)
    {
        if(obj == NULL)
        {
            GYRO_ERR("null pointer!!\n");
            return -EINVAL;
        }
        atomic_set(&obj->suspend, 1);

        err = L3GD20_SetPowerMode(client, false);
        if(err <= 0)
        {
            return err;
        }
    }
    return err;
}
/*----------------------------------------------------------------------------*/
static int l3gd20_resume(struct i2c_client *client)
{
    struct l3gd20_i2c_data *obj = i2c_get_clientdata(client);
    int err;
    GYRO_FUN();

    if(obj == NULL)
    {
        GYRO_ERR("null pointer!!\n");
        return -EINVAL;
    }

    L3GD20_power(obj->hw, 1);
    err = l3gd20_init_client(client, false);
    if(err)
    {
        GYRO_ERR("initialize client fail!!\n");
        return err;
    }
    atomic_set(&obj->suspend, 0);

    return 0;
}
/*----------------------------------------------------------------------------*/
#else /*CONFIG_HAS_EARLY_SUSPEND is defined*/
/*----------------------------------------------------------------------------*/
static void l3gd20_early_suspend(struct early_suspend *h)
{
    struct l3gd20_i2c_data *obj = container_of(h, struct l3gd20_i2c_data, early_drv);
    int err;
    GYRO_FUN();

    if(obj == NULL)
    {
        GYRO_ERR("null pointer!!\n");
        return;
    }
    atomic_set(&obj->suspend, 1);
    err = L3GD20_SetPowerMode(obj->client, false);
    if(err)
    {
        GYRO_ERR("write power control fail!!\n");
        return;
    }
    if(err <= 0)
    {
        return;
    }

    sensor_power = false;

    L3GD20_power(obj->hw, 0);
}
/*----------------------------------------------------------------------------*/
static void l3gd20_late_resume(struct early_suspend *h)
{
    struct l3gd20_i2c_data *obj = container_of(h, struct l3gd20_i2c_data, early_drv);
    int err;
    GYRO_FUN();

    if(obj == NULL)
    {
        GYRO_ERR("null pointer!!\n");
        return;
    }

    L3GD20_power(obj->hw, 1);
    err = l3gd20_init_client(obj->client, false);
    if(err)
    {
        GYRO_ERR("initialize client fail! err code %d!\n", err);
        return;
    }
    atomic_set(&obj->suspend, 0);
}
/*----------------------------------------------------------------------------*/
#endif /*CONFIG_HAS_EARLYSUSPEND*/
/*----------------------------------------------------------------------------*/

static int l3gd20_open_report_data(int open)
{
    return 0;
}

static int l3gd20_enable_nodata(int en)
{
    int res =0;
    int retry = 0;
    bool power=false;

    if(1==en)
    {
        power=true;
    }
    if(0==en)
    {
        power =false;
    }

    for(retry = 0; retry < 3; retry++){
        res = L3GD20_SetPowerMode(obj_i2c_data->client, power);
        if(res == 0)
        {
            GYRO_LOG("L3GD20_SetPowerMode done\n");
            break;
        }
        GYRO_LOG("L3GD20_SetPowerMode fail\n");
    }


    if(res != L3GD20_SUCCESS)
    {
        GYRO_LOG("L3GD20_SetPowerMode fail!\n");
        return -1;
    }
    GYRO_LOG("l3gd20_enable_nodata OK!\n");
    return 0;

}

static int l3gd20_set_delay(u64 ns)
{
    return 0;
}

static int l3gd20_get_data(int* x ,int* y,int* z, int* status)
{
    char buff[L3GD20_BUFSIZE];
    L3GD20_ReadGyroData(obj_i2c_data->client, buff, L3GD20_BUFSIZE);

    sscanf(buff, "%x %x %x", x, y, z);
    *status = SENSOR_STATUS_ACCURACY_MEDIUM;

    return 0;
}


/*----------------------------------------------------------------------------*/
static int l3gd20_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct i2c_client *new_client;
    struct l3gd20_i2c_data *obj;
    struct gyro_control_path ctl={0};
    struct gyro_data_path data={0};

    int err = 0;
    GYRO_FUN();

    if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
    {
        err = -ENOMEM;
        goto exit;
    }

    memset(obj, 0, sizeof(struct l3gd20_i2c_data));

    obj->hw = l3gd20_get_cust_gyro_hw();
    err = hwmsen_get_convert(obj->hw->direction, &obj->cvt);
    if(err)
    {
        GYRO_ERR("invalid direction: %d\n", obj->hw->direction);
        goto exit;
    }

    obj_i2c_data = obj;
    obj->client = client;
    new_client = obj->client;
    i2c_set_clientdata(new_client,obj);

    atomic_set(&obj->trace, 0);
    atomic_set(&obj->suspend, 0);



    l3gd20_i2c_client = new_client;
    err = l3gd20_init_client(new_client, false);
    if(err)
    {
        goto exit_init_failed;
    }


    err = misc_register(&l3gd20_device);
    if(err)
    {
        GYRO_ERR("l3gd20_device misc register failed!\n");
        goto exit_misc_device_register_failed;
    }

    err = l3gd20_create_attr(&(l3gd20_init_info.platform_diver_addr->driver));
    if(err)
    {
        GYRO_ERR("l3gd20 create attribute err = %d\n", err);
        goto exit_create_attr_failed;
    }

    ctl.open_report_data= l3gd20_open_report_data;
    ctl.enable_nodata = l3gd20_enable_nodata;
    ctl.set_delay  = l3gd20_set_delay;
    ctl.is_report_input_direct = false;
    ctl.is_support_batch = obj->hw->is_batch_supported;

    err = gyro_register_control_path(&ctl);
    if(err)
    {
         GYRO_ERR("register gyro control path err\n");
        goto exit_kfree;
    }

    data.get_data = l3gd20_get_data;
    data.vender_div = DEGREE_TO_RAD;
    err = gyro_register_data_path(&data);
    if(err)
        {
           GYRO_ERR("gyro_register_data_path fail = %d\n", err);
           goto exit_kfree;
        }
    err = batch_register_support_info(ID_GYROSCOPE,obj->hw->is_batch_supported);
    if(err)
    {
        GYRO_ERR("register gyro batch support err = %d\n", err);
        goto exit_kfree;
    }



#ifdef CONFIG_HAS_EARLYSUSPEND
    obj->early_drv.level    = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 2,
    obj->early_drv.suspend  = l3gd20_early_suspend,
    obj->early_drv.resume   = l3gd20_late_resume,
    register_early_suspend(&obj->early_drv);
#endif
    l3gd20_init_flag = 0;
    GYRO_LOG("%s: OK\n", __func__);
    return 0;

    exit_create_attr_failed:
    misc_deregister(&l3gd20_device);
    exit_misc_device_register_failed:
    exit_init_failed:
    exit_kfree:
    kfree(obj);
    exit:
    l3gd20_init_flag = 0;
    GYRO_ERR("%s: err = %d\n", __func__, err);
    return err;
}

/*----------------------------------------------------------------------------*/
static int l3gd20_i2c_remove(struct i2c_client *client)
{
    int err = 0;

    err = l3gd20_delete_attr(&(l3gd20_init_info.platform_diver_addr->driver));
    if(err)
    {
        GYRO_ERR("l3gd20_delete_attr fail: %d\n", err);
    }

    err = misc_deregister(&l3gd20_device);
    if(err)
    {
        GYRO_ERR("misc_deregister fail: %d\n", err);
    }

    l3gd20_i2c_client = NULL;
    i2c_unregister_device(client);
    kfree(i2c_get_clientdata(client));
    return 0;
}



/*----------------------------------------------------------------------------*/
static int l3gd20_remove(void)
{
    struct gyro_hw *hw = l3gd20_get_cust_gyro_hw();
    GYRO_FUN();
    L3GD20_power(hw, 0);
    i2c_del_driver(&l3gd20_i2c_driver);

    return 0;
}

static int l3gd20_local_init(void)
{
    struct gyro_hw *hw = l3gd20_get_cust_gyro_hw();

    L3GD20_power(hw, 1);
    if(i2c_add_driver(&l3gd20_i2c_driver))
    {
        GYRO_ERR("add driver error\n");
        return -1;
    }
    if(-1 == l3gd20_init_flag)
    {
       return -1;
    }
    return 0;
}
static int update_gyro_data(void)
{
    struct gyro_hw_ssb *l3gd20_gyro_data =NULL;
    const char *name = "l3gd20";
    int err = 0;

    if ((l3gd20_gyro_data = find_gyro_data(name))) {
        l3gd20_get_cust_gyro_hw()->addr  = l3gd20_gyro_data->i2c_addr;
        l3gd20_get_cust_gyro_hw()->i2c_num   = l3gd20_gyro_data->i2c_num;
        l3gd20_get_cust_gyro_hw()->direction = l3gd20_gyro_data->direction;
        l3gd20_get_cust_gyro_hw()->firlen    = l3gd20_gyro_data->firlen;
        GYRO_LOG("[%s]l3gd20 success update addr=0x%x,i2c_num=%d,direction=%d\n",
        __func__,l3gd20_gyro_data->i2c_addr,l3gd20_gyro_data->i2c_num,l3gd20_gyro_data->direction);
    }
    return err;
}
static int __init l3gd20_init(void)
{
    struct gyro_hw *hw = NULL;
    update_gyro_data();
    hw = l3gd20_get_cust_gyro_hw();
    struct i2c_board_info i2c_l3gd20={ I2C_BOARD_INFO(L3GD20_DEV_NAME, hw->addr)};
    i2c_register_board_info(hw->i2c_num, &i2c_l3gd20, 1);
    gyro_driver_add(&l3gd20_init_info);

    return 0;
}

/*----------------------------------------------------------------------------*/
static void __exit l3gd20_exit(void)
{
    GYRO_FUN();
}
/*----------------------------------------------------------------------------*/
module_init(l3gd20_init);
module_exit(l3gd20_exit);
/*----------------------------------------------------------------------------*/
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("L3GD20 gyroscope driver");
MODULE_AUTHOR("Chunlei.Wang@mediatek.com");
