/*
 *  Player - One Hell of a Robot Server
 *  Copyright (C) 2008  Willow Garage
 *                      
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <netinet/in.h>
#include <stdlib.h>

#include <sys/time.h>

//#include <ros/console.h>

#include "3dmgx2_driver/3dmgx2.h"

#include "poll.h"


//! Macro for throwing an exception with a message
#define IMU_EXCEPT(except, msg, ...) \
  { \
    char buf[100]; \
    snprintf(buf, 100, "ms_3dmgx2_driver::IMU::%s: " msg, __FUNCTION__,##__VA_ARGS__); \
    throw except(buf); \
  }


//! Code to swap bytes since IMU is big endian
static inline unsigned short bswap_16(unsigned short x) {
  return (x>>8) | (x<<8);
}

//! Code to swap bytes since IMU is big endian
static inline unsigned int bswap_32(unsigned int x) {
  return (bswap_16(x&0xffff)<<16) | (bswap_16(x>>16));
}


//! Code to extract a floating point number from the IMU
static float extract_float(uint8_t* addr) {

  float tmp;

  *((unsigned char*)(&tmp) + 3) = *(addr);
  *((unsigned char*)(&tmp) + 2) = *(addr+1);
  *((unsigned char*)(&tmp) + 1) = *(addr+2);
  *((unsigned char*)(&tmp)) = *(addr+3);

  return tmp;
}


//! Helper function to get system time in nanoseconds.
static unsigned long long time_helper()
{
#if POSIX_TIMERS > 0
  struct timespec curtime;
  clock_gettime(CLOCK_REALTIME, &curtime);
  return (unsigned long long)(curtime.tv_sec) * 1000000000 + (unsigned long long)(curtime.tv_nsec);  
#else
  struct timeval timeofday;
  gettimeofday(&timeofday,NULL);
  return (unsigned long long)(timeofday.tv_sec) * 1000000000 + (unsigned long long)(timeofday.tv_usec) * 1000;  
#endif
}


////////////////////////////////////////////////////////////////////////////////
// Constructor
ms_3dmgx2_driver::IMU::IMU() : fd(-1), continuous(false)
{ }


////////////////////////////////////////////////////////////////////////////////
// Destructor
ms_3dmgx2_driver::IMU::~IMU()
{
  closePort();
}


////////////////////////////////////////////////////////////////////////////////
// Open the IMU port
void
ms_3dmgx2_driver::IMU::openPort(const char *port_name)
{
  // Open the port
  fd = open(port_name, O_RDWR | O_SYNC , S_IRUSR | S_IWUSR );
  if (fd < 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Unable to open serial port [%s]; [%s]", port_name, strerror(errno));

  // Change port settings
  struct termios term;
  if (tcgetattr(fd, &term) < 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Unable to get serial port attributes");

  cfmakeraw( &term );
  cfsetispeed(&term, B115200);
  cfsetospeed(&term, B115200);

  if (tcsetattr(this->fd, TCSAFLUSH, &term) < 0 )
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Unable to set serial port attributes");

  // Stop continuous mode
  stopContinuous();

  // Make sure queues are empty before we begin
  if (tcflush(fd, TCIOFLUSH) != 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Tcflush failed");
}


////////////////////////////////////////////////////////////////////////////////
// Close the IMU port
void
ms_3dmgx2_driver::IMU::closePort()
{
  try {
    stopContinuous();

  } catch (ms_3dmgx2_driver::Exception &e) {
    // Exceptions here are fine since we are closing anyways
  }
  
  if (fd != -1)
    if (close(fd) != 0)
      IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Unable to close serial port; [%s]", strerror(errno));
  fd = -1;
}



////////////////////////////////////////////////////////////////////////////////
// Initialize time information
void
ms_3dmgx2_driver::IMU::initTime(double fix_off)
{
  wraps = 0;

  uint8_t cmd[1];
  uint8_t rep[31];
  cmd[0] = CMD_RAW;

  transact(cmd, sizeof(cmd), rep, sizeof(rep));
  start_time = time_helper();

  int k = 25;
  offset_ticks = bswap_32(*(uint32_t*)(rep + k));
  last_ticks = offset_ticks;

  // reset kalman filter state
  offset = 0;
  d_offset = 0;
  sum_meas = 0;
  counter = 0;

  // fixed offset
  fixed_offset = fix_off;
}

////////////////////////////////////////////////////////////////////////////////
// Initialize IMU gyros
void
ms_3dmgx2_driver::IMU::initGyros(double* bias_x, double* bias_y, double* bias_z)
{
  wraps = 0;

  uint8_t cmd[5];
  uint8_t rep[19];

  cmd[0] = CMD_CAPTURE_GYRO_BIAS;
  cmd[1] = 0xC1;
  cmd[2] = 0x29;
  *(unsigned short*)(&cmd[3]) = bswap_16(10000);

  transact(cmd, sizeof(cmd), rep, sizeof(rep));

  if (bias_x)
    *bias_x = extract_float(rep + 1);
  
  if (bias_y)
    *bias_y = extract_float(rep + 5);

  if (bias_z)
    *bias_z = extract_float(rep + 9);
}


////////////////////////////////////////////////////////////////////////////////
// Put the IMU into continuous mode
bool
ms_3dmgx2_driver::IMU::setContinuous(cmd command)
{
  uint8_t cmd[4];
  uint8_t rep[8];

  cmd[0] = CMD_CONTINUOUS;
  cmd[1] = 0xC1; //Confirms user intent
  cmd[2] = 0x29; //Confirms user intent
  cmd[3] = command;

  transact(cmd, sizeof(cmd), rep, sizeof(rep));
  
  // Verify that continuous mode is set on correct command:
  if (rep[1] != command) {
    return false;
  }

  continuous = true;
  return true;
}


////////////////////////////////////////////////////////////////////////////////
// Take the IMU out of continuous mode
void
ms_3dmgx2_driver::IMU::stopContinuous()
{
  uint8_t cmd[1];

  cmd[0] = CMD_STOP_CONTINUOUS;
  
  send(cmd, sizeof(cmd));

  usleep(1000000);

  if (tcflush(fd, TCIOFLUSH) != 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "Tcflush failed");
}



////////////////////////////////////////////////////////////////////////////////
// Receive ACCEL_ANGRATE_MAG message
void
ms_3dmgx2_driver::IMU::receiveAccelAngrateMag(uint64_t *time, double accel[3], double angrate[3], double mag[3])
{
  int i, k;
  uint8_t rep[43];

  uint64_t sys_time;
  uint64_t imu_time;

  //ROS_DEBUG("About to do receive.");
  receive(CMD_ACCEL_ANGRATE_MAG, rep, sizeof(rep), 0, &sys_time);
  //ROS_DEBUG("Receive finished.");

  // Read the acceleration:
  k = 1;
  for (i = 0; i < 3; i++)
  {
    accel[i] = extract_float(rep + k) * G;
    k += 4;
  }

  // Read the angular rates
  k = 13;
  for (i = 0; i < 3; i++)
  {
    angrate[i] = extract_float(rep + k);
    k += 4;
  }

  // Read the orientation matrix
  k = 25;
  for (i = 0; i < 3; i++) {
    mag[i] = extract_float(rep + k);
    k += 4;
  }

  imu_time = extractTime(rep+37);
  *time = filterTime(imu_time, sys_time);
}

////////////////////////////////////////////////////////////////////////////////
// Receive ACCEL_ANGRATE_ORIENTATION message
void
ms_3dmgx2_driver::IMU::receiveAccelAngrateOrientation(uint64_t *time, double accel[3], double angrate[3], double orientation[9])
{
  int i, k;
  uint8_t rep[67];

  uint64_t sys_time;
  uint64_t imu_time;

  //ROS_DEBUG("About to do receive.");
  receive(CMD_ACCEL_ANGRATE_ORIENT, rep, sizeof(rep), 0, &sys_time);
  //ROS_DEBUG("Finished receive.");

  // Read the acceleration:
  k = 1;
  for (i = 0; i < 3; i++)
  {
    accel[i] = extract_float(rep + k) * G;
    k += 4;
  }

  // Read the angular rates
  k = 13;
  for (i = 0; i < 3; i++)
  {
    angrate[i] = extract_float(rep + k);
    k += 4;
  }

  // Read the orientation matrix
  k = 25;
  for (i = 0; i < 9; i++) {
    orientation[i] = extract_float(rep + k);
    k += 4;
  }

  imu_time = extractTime(rep+61);
  *time = filterTime(imu_time, sys_time);
}


////////////////////////////////////////////////////////////////////////////////
// Receive ACCEL_ANGRATE message
void
ms_3dmgx2_driver::IMU::receiveAccelAngrate(uint64_t *time, double accel[3], double angrate[3])
{
  int i, k;
  uint8_t rep[31];

  uint64_t sys_time;
  uint64_t imu_time;

  receive(CMD_ACCEL_ANGRATE, rep, sizeof(rep), 0, &sys_time);

  // Read the acceleration:
  k = 1;
  for (i = 0; i < 3; i++)
  {
    accel[i] = extract_float(rep + k) * G;
    k += 4;
  }

  // Read the angular rates
  k = 13;
  for (i = 0; i < 3; i++)
  {
    angrate[i] = extract_float(rep + k);
    k += 4;
  }

  imu_time = extractTime(rep+25);
  *time = filterTime(imu_time, sys_time);
}


////////////////////////////////////////////////////////////////////////////////
// Receive EULER message
void
ms_3dmgx2_driver::IMU::receiveEuler(uint64_t *time, double *roll, double *pitch, double *yaw)
{
  uint8_t rep[19];

  uint64_t sys_time;
  uint64_t imu_time;

  receive(CMD_EULER, rep, sizeof(rep), 0, &sys_time);

  *roll  = extract_float(rep + 1);
  *pitch = extract_float(rep + 5);
  *yaw   = extract_float(rep + 9);

  imu_time  = extractTime(rep + 13);
  *time = filterTime(imu_time, sys_time);
}
    
////////////////////////////////////////////////////////////////////////////////
// Receive Device Identifier String

bool ms_3dmgx2_driver::IMU::getDeviceIdentifierString(id_string type, char id[17])
{
  uint8_t cmd[2];
  uint8_t rep[20];

  cmd[0] = CMD_DEV_ID_STR;
  cmd[1] = type;

  transact(cmd, sizeof(cmd), rep, sizeof(rep));
  
  if (cmd[0] != CMD_DEV_ID_STR || cmd[1] != type)
    return false;

  id[16] = 0;
  memcpy(id, rep+2, 16);

  return true;
}

/* ideally it would be nice to feed these functions back into willowimu */
#define CMD_ACCEL_ANGRATE_MAG_ORIENT_REP_LEN 79
#define CMD_RAW_ACCEL_ANGRATE_LEN 31
////////////////////////////////////////////////////////////////////////////////
// Receive ACCEL_ANGRATE_MAG_ORIENT message
void 
ms_3dmgx2_driver::IMU::receiveAccelAngrateMagOrientation (uint64_t *time, double accel[3], double angrate[3], double mag[3], double orientation[9]) 
{
	uint8_t	 rep[CMD_ACCEL_ANGRATE_MAG_ORIENT_REP_LEN];

	int k, i;
	uint64_t sys_time;
	uint64_t imu_time;

	receive( CMD_ACCEL_ANGRATE_MAG_ORIENT, rep, sizeof(rep), 0, &sys_time); 

  // Read the acceleration:
  k = 1;
  for (i = 0; i < 3; i++)
  {
    accel[i] = extract_float(rep + k) * G;
    k += 4;
  }

  // Read the angular rates
  k = 13;
  for (i = 0; i < 3; i++)
  {
    angrate[i] = extract_float(rep + k);
    k += 4;
  }

  // Read the magnetic field matrix
  k = 25;
  for (i = 0; i < 3; i++) {
    mag[i] = extract_float(rep + k);
    k += 4;
  }

 // Read the orientation matrix
  k = 37;
  for (i = 0; i < 9; i++) {
    orientation[i] = extract_float(rep + k);
    k += 4;
  }
	imu_time  = extractTime(rep + 73);

	*time = filterTime(imu_time, sys_time);
}

////////////////////////////////////////////////////////////////////////////////
// Receive RAW message
// (copy of receive accel angrate but with raw cmd)
void
ms_3dmgx2_driver::IMU::receiveRawAccelAngrate(uint64_t *time, double accel[3], double angrate[3])
{
  int i, k;
  uint8_t rep[CMD_RAW_ACCEL_ANGRATE_LEN];

  uint64_t sys_time;
  uint64_t imu_time;

  receive(ms_3dmgx2_driver::IMU::CMD_RAW, rep, sizeof(rep), 0, &sys_time);

  // Read the accelerator AD register values 0 - 65535 given as float
  k = 1;
  for (i = 0; i < 3; i++)
  {
    accel[i] = extract_float(rep + k);
    k += 4;
  }

  // Read the angular rates AD registor values 0 - 65535 (given as float
  k = 13;
  for (i = 0; i < 3; i++)
  {
    angrate[i] = extract_float(rep + k);
    k += 4;
  }

  imu_time = extractTime(rep+25);
  *time = filterTime(imu_time, sys_time);
}


////////////////////////////////////////////////////////////////////////////////
// Extract time and process rollover
uint64_t
ms_3dmgx2_driver::IMU::extractTime(uint8_t* addr)
{
  uint32_t ticks = bswap_32(*(uint32_t*)(addr));

  if (ticks < last_ticks) {
    wraps += 1;
  }

  last_ticks = ticks;

  uint64_t all_ticks = ((uint64_t)wraps << 32) - offset_ticks + ticks;

  return  start_time + (uint64_t)(all_ticks * (1000000000.0 / TICKS_PER_SEC));
}



////////////////////////////////////////////////////////////////////////////////
// Send a packet and wait for a reply from the IMU.
// Returns the number of bytes read.
int ms_3dmgx2_driver::IMU::transact(void *cmd, int cmd_len, void *rep, int rep_len, int timeout)
{
  send(cmd, cmd_len);
  
  return receive(*(uint8_t*)cmd, rep, rep_len, timeout);
}


////////////////////////////////////////////////////////////////////////////////
// Send a packet to the IMU.
// Returns the number of bytes written.
int
ms_3dmgx2_driver::IMU::send(void *cmd, int cmd_len)
{
  int bytes;

  // Write the data to the port
  bytes = write(this->fd, cmd, cmd_len);
  if (bytes < 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "error writing to IMU [%s]", strerror(errno));

  if (bytes != cmd_len)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "whole message not written to IMU");

  // Make sure the queue is drained
  // Synchronous IO doesnt always work
  if (tcdrain(this->fd) != 0)
    IMU_EXCEPT(ms_3dmgx2_driver::Exception, "tcdrain failed");

  return bytes;
}


////////////////////////////////////////////////////////////////////////////////
// Receive a reply from the IMU.
// Returns the number of bytes read.
int
ms_3dmgx2_driver::IMU::receive(uint8_t command, void *rep, int rep_len, int timeout, uint64_t* sys_time)
{
  int nbytes, bytes, skippedbytes, retval;

  skippedbytes = 0;

  struct pollfd ufd[1];
  ufd[0].fd = fd;
  ufd[0].events = POLLIN;
  
  // Skip everything until we find our "header"
  *(uint8_t*)(rep) = 0;
  
  while (*(uint8_t*)(rep) != command && skippedbytes < MAX_BYTES_SKIPPED)
  {
    if (timeout > 0)
    {
      if ( (retval = poll(ufd, 1, timeout)) < 0 )
        IMU_EXCEPT(ms_3dmgx2_driver::Exception, "poll failed  [%s]", strerror(errno));
      
      if (retval == 0)
        IMU_EXCEPT(ms_3dmgx2_driver::TimeoutException, "timeout reached");
    }
	
    if (read(this->fd, (uint8_t*) rep, 1) <= 0)
      IMU_EXCEPT(ms_3dmgx2_driver::Exception, "read failed [%s]", strerror(errno));

    skippedbytes++;
  }

  if (sys_time != NULL)
    *sys_time = time_helper();
  
  // We now have 1 byte
  bytes = 1;

  // Read the rest of the message:
  while (bytes < rep_len)
  {
    if (timeout > 0)
    {
      if ( (retval = poll(ufd, 1, timeout)) < 0 )
        IMU_EXCEPT(ms_3dmgx2_driver::Exception, "poll failed  [%s]", strerror(errno));
      
      if (retval == 0)
        IMU_EXCEPT(ms_3dmgx2_driver::TimeoutException, "timeout reached");
    }

    nbytes = read(this->fd, (uint8_t*) rep + bytes, rep_len - bytes);

    if (nbytes < 0)
      IMU_EXCEPT(ms_3dmgx2_driver::Exception, "read failed  [%s]", strerror(errno));
    
    bytes += nbytes;
  }

  // Checksum is always final 2 bytes of transaction

  uint16_t checksum = 0;
  for (int i = 0; i < rep_len - 2; i++) {
    checksum += ((uint8_t*)rep)[i];
  }

  // If wrong throw Exception
  if (checksum != bswap_16(*(uint16_t*)((uint8_t*)rep+rep_len-2)))
    IMU_EXCEPT(ms_3dmgx2_driver::CorruptedDataException, "invalid checksum.\n Make sure the IMU sensor is connected to this computer.");
  
  return bytes;
}

////////////////////////////////////////////////////////////////////////////////
// Kalman filter for time estimation
uint64_t ms_3dmgx2_driver::IMU::filterTime(uint64_t imu_time, uint64_t sys_time)
{
  // first calculate the sum of KF_NUM_SUM measurements
  if (counter < KF_NUM_SUM){
    counter ++;
    sum_meas += (toDouble(imu_time) - toDouble(sys_time));
  }
  // update kalman filter with fixed innovation
  else{
    // system update
    offset += d_offset;

    // measurement update
    double meas_diff = (sum_meas/KF_NUM_SUM) - offset;
    offset   += KF_K_1 * meas_diff;
    d_offset += KF_K_2 * meas_diff;

    // reset counter and average
    counter = 0; sum_meas = 0;
  }
  return imu_time - toUint64_t( offset ) + toUint64_t( fixed_offset );
}


////////////////////////////////////////////////////////////////////////////////
// convert uint64_t time to double time
double ms_3dmgx2_driver::IMU::toDouble(uint64_t time)
{
  double res = trunc(time/1e9);
  res += (((double)time)/1e9) - res;
  return res;
}


////////////////////////////////////////////////////////////////////////////////
// convert double time to uint64_t time
uint64_t  ms_3dmgx2_driver::IMU::toUint64_t(double time)
{
  return (uint64_t)(time * 1e9);
}
