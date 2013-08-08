/*
 * i2c_dev.c
 *
 *  Created on: Aug 7, 2013
 *      Author: petera
 */

#include "i2c_dev.h"
#include "miniutils.h"

#define I2C_DEV_BUS_USER_ARG_BUSY_BIT       (1<<0)

static void i2c_dev_reset(i2c_dev *dev) {
  dev->bus->user_arg &= ~I2C_DEV_BUS_USER_ARG_BUSY_BIT;
}

static void i2c_dev_config(i2c_bus *bus, u32_t clock_config) {
  I2C_config(bus, clock_config);
}

static void i2c_dev_finish(i2c_dev *dev, int res) {
  DBG(D_I2C, D_DEBUG, "i2c_dev: operation finished %i\n", res);
  i2c_dev_reset(dev);
  if (dev->i2c_dev_callback) {
    // todo : move to task if necessary
    dev->i2c_dev_callback(dev, res);
  }
}

static void i2c_dev_exec(i2c_dev *dev) {
  ASSERT(dev != NULL);

  dev->bus->user_p = dev;
  // grab next sequence
  if (dev->seq_len > 0) {
    memcpy(&dev->cur_seq, dev->seq_list, sizeof(i2c_dev_sequence));
    dev->seq_len--;
    dev->seq_list++;
    DBG(D_I2C, D_DEBUG, "i2c_dev: running seq %s, len %i, addr:%08x, gen_stop:%i\n",
        dev->cur_seq.dir ? "TX" : "RX",
            dev->cur_seq.len,
            dev->cur_seq.buf,
            dev->cur_seq.gen_stop);

  } else {
    DBG(D_I2C, D_DEBUG, "i2c_dev: sequencing finished ok\n");
    i2c_dev_finish(dev, I2C_OK);
    return;
  }

  // force stop if this is the last sequence
  bool gen_stop = dev->cur_seq.gen_stop | (dev->seq_len == 0);
  int res = dev->cur_seq.dir ?
      I2C_tx(dev->bus, dev->addr, dev->cur_seq.buf, dev->cur_seq.len, gen_stop) :
      I2C_rx(dev->bus, dev->addr, dev->cur_seq.buf, dev->cur_seq.len, gen_stop);
  if (res != I2C_OK) {
    DBG(D_I2C, D_DEBUG, "i2c_dev: sequence call failed %i\n", res);
    i2c_dev_finish(dev, res);
  }
}

static void i2c_dev_callback_irq(i2c_bus *bus, int res) {
  ASSERT(bus != NULL);
  i2c_dev *dev = (i2c_dev *)bus->user_p;
  if (res < I2C_OK) {
    DBG(D_I2C, D_DEBUG, "i2c_dev: irq - fail\n");

    i2c_dev_finish(dev, res);
  } else {
    DBG(D_I2C, D_DEBUG, "i2c_dev: irq - ok\n");

    i2c_dev_exec(dev);
  }
}

void I2C_DEV_init(i2c_dev *dev, u32_t clock, i2c_bus *bus, u8_t addr) {
  memset(dev, 0, sizeof(i2c_dev));
  dev->clock_configuration = clock;
  dev->bus = bus;
  dev->addr = addr;

  I2C_set_callback(dev->bus, i2c_dev_callback_irq);
}

void I2C_DEV_open(i2c_dev *dev) {
  if (!dev->opened) {
    I2C_register(dev->bus);
    i2c_dev_reset(dev);
    dev->opened = TRUE;
  }
}

int I2C_DEV_sequence(i2c_dev *dev, i2c_dev_sequence *seq, u8_t seq_len) {
  if (dev->bus->state != I2C_S_IDLE) {
    return I2C_ERR_BUS_BUSY;
  }
  if (dev->bus->user_arg & I2C_DEV_BUS_USER_ARG_BUSY_BIT) {
    return I2C_ERR_DEV_BUSY;
  }

  dev->bus->user_arg |= I2C_DEV_BUS_USER_ARG_BUSY_BIT;

  if (dev->bus->user_p == NULL || ((i2c_dev *)dev->bus->user_p)->clock_configuration != dev->clock_configuration) {
    // last bus use wasn't this device, so reconfigure
    i2c_dev_config(dev->bus, dev->clock_configuration);
  }

  dev->seq_len = seq_len;
  dev->seq_list = seq;
  dev->cur_seq.buf = 0;
  dev->cur_seq.len = 0;
  dev->cur_seq.gen_stop = 0;
  dev->cur_seq.dir = 0;

  i2c_dev_exec(dev);

  return I2C_OK;
}

int I2C_DEV_set_callback(i2c_dev *dev, i2c_dev_callback cb) {
  if (dev->bus->user_arg & I2C_DEV_BUS_USER_ARG_BUSY_BIT) {
    return I2C_ERR_DEV_BUSY;
  }
  dev->i2c_dev_callback = cb;
  return I2C_OK;

}

void I2C_DEV_close(i2c_dev *dev) {
  if (dev->opened) {
    i2c_dev_reset(dev);
    I2C_release(dev->bus);
    dev->opened = FALSE;
  }
}

bool I2C_DEV_is_busy(i2c_dev *dev) {
  return I2C_is_busy(dev->bus) | ((dev->bus->user_arg & I2C_DEV_BUS_USER_ARG_BUSY_BIT) != 0);
}
