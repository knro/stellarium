/*
 * Copyright (C) 2017 Alessandro Siniscalchi <asiniscalchi@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "INDIConnection.hpp"

#include <QDebug>
#include <QString>
#include <chrono>
#include <thread>
#include <limits>
#include <cmath>

#include "libindi/baseclient.h"
#include "libindi/basedevice.h"
#include "libindi/inditelescope.h"

const int INDIConnection::SLEW_STOP = INDI::Telescope::SLEW_GUIDE - 1;

INDIConnection::INDIConnection(QObject *parent) : QObject(parent)
{
}

INDIConnection::Coordinates INDIConnection::position() const
{
   std::lock_guard<std::mutex> lock(mMutex);
   return mCoordinates;
}

void INDIConnection::setPosition(INDIConnection::Coordinates coords)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope)
      return;

   if (!mTelescope->isConnected())
   {
      qDebug() << "Error: Telescope not connected";
      return;
   }

   // Make sure the TRACK member of switch ON_COORD_SET is set
   ISwitchVectorProperty *switchVector = mTelescope->getSwitch("ON_COORD_SET");
   if (!switchVector)
   {
      qDebug() << "Error: unable to find Telescope or ON_COORD_SET switch...";
      return;
   }
   // Note that confusingly there is a SLEW switch member as well that will move but not track.
   // TODO: Figure out if there is to be support for it
   ISwitch *track = IUFindSwitch(switchVector, "TRACK");
   if (track->s == ISS_OFF)
   {
      track->s = ISS_ON;
      sendNewSwitch(switchVector);
   }

   INumberVectorProperty *property = nullptr;
   property = mTelescope->getNumber("EQUATORIAL_EOD_COORD");
   if (!property)
   {
      qDebug() << "Error: unable to find Telescope or EQUATORIAL_EOD_COORD property...";
      return;
   }

   property->np[0].value = coords.RA;
   property->np[1].value = coords.DEC;
   sendNewNumber(property);
}

void INDIConnection::syncPosition(INDIConnection::Coordinates coords)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope)
      return;

   if (!mTelescope->isConnected())
   {
      qDebug() << "Error: Telescope not connected";
      return;
   }

   // Make sure the SYNC member of switch ON_COORD_SET is set
   ISwitchVectorProperty *switchVector = mTelescope->getSwitch("ON_COORD_SET");
   if (!switchVector)
   {
      qDebug() << "Error: unable to find Telescope or ON_COORD_SET switch...";
      return;
   }

   ISwitch *track = IUFindSwitch(switchVector, "TRACK");
   ISwitch *slew = IUFindSwitch(switchVector, "SLEW");
   ISwitch *sync = IUFindSwitch(switchVector, "SYNC");
   track->s = ISS_OFF;
   slew->s = ISS_OFF;
   sync->s = ISS_ON;
   sendNewSwitch(switchVector);

   INumberVectorProperty *property = nullptr;
   property = mTelescope->getNumber("EQUATORIAL_EOD_COORD");
   if (!property)
   {
      qDebug() << "Error: unable to find Telescope or EQUATORIAL_EOD_COORD property...";
      return;
   }

   property->np[0].value = coords.RA;
   property->np[1].value = coords.DEC;
   sendNewNumber(property);

   // And now unset SYNC switch member to revert to default state/behavior
   track->s = ISS_ON;
   slew->s = ISS_OFF;
   sync->s = ISS_OFF;
   sendNewSwitch(switchVector);
}

bool INDIConnection::isDeviceConnected() const
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope)
      return false;

   return mTelescope.isConnected();
}

const QStringList INDIConnection::devices() const
{
   std::lock_guard<std::mutex> lock(mMutex);
   return mDevices;
}

void INDIConnection::unParkTelescope()
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope.isConnected())
      return;

   auto svp = mTelescope.getSwitch("TELESCOPE_PARK");
   if (!svp.isValid())
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_PARK switch...";
      return;
   }

   svp.reset();
   auto unparkS = svp.findWidgetByName("UNPARK");
   unparkS->setState(ISS_ON);
   sendNewProperty(svp);
}

/*
 * Unused at the moment
 * TODO: Enable method after changes in the GUI
void INDIConnection::parkTelescope()
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope->isConnected())
      return;

   ISwitchVectorProperty *switchVector = mTelescope->getSwitch("TELESCOPE_PARK");
   if (!switchVector)
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_PARK switch...";
      return;
   }

   ISwitch *park = IUFindSwitch(switchVector, "PARK");
   if (park->s == ISS_OFF)
   {
      park->s = ISS_ON;
      sendNewSwitch(switchVector);
   }

   ISwitch *unpark = IUFindSwitch(switchVector, "UNPARK");
   if (unpark->s == ISS_ON)
   {
      unpark->s = ISS_OFF;
      sendNewSwitch(switchVector);
   }
}
*/

void INDIConnection::moveNorth(int speed)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope.isConnected())
      return;

   auto svp = mTelescope.getSwitch("TELESCOPE_MOTION_NS");
   if (!svp.isValid())
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_MOTION_NS switch...";
      return;
   }

   auto motion = svp.findWidgetByName("MOTION_NORTH");

   if (speed == SLEW_STOP)
      motion->setState(ISS_OFF);
   else
   {
      setSpeed(speed);
      motion->setState(ISS_ON);
   }

   sendNewProperty(svp);
}

void INDIConnection::moveEast(int speed)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope.isConnected())
      return;

   auto svp = mTelescope.getSwitch("TELESCOPE_MOTION_WE");
   if (!svp.isValid())
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_MOTION_WE switch...";
      return;
   }

   auto motion = svp.findWidgetByName("MOTION_EAST");

   if (speed == SLEW_STOP)
      motion->setState(ISS_OFF);
   else
   {
      setSpeed(speed);
      motion->setState(ISS_ON);
   }

   sendNewProperty(svp);
}

void INDIConnection::moveSouth(int speed)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope.isConnected())
      return;

   auto svp = mTelescope.getSwitch("TELESCOPE_MOTION_NS");
   if (!svp.isValid())
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_MOTION_NS switch...";
      return;
   }

   auto motion = svp.findWidgetByName("MOTION_SOUTH");

   if (speed == SLEW_STOP)
      motion->setState(ISS_OFF);
   else
   {
      setSpeed(speed);
      motion->setState(ISS_ON);
   }

   sendNewProperty(svp);
}

void INDIConnection::moveWest(int speed)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!mTelescope || !mTelescope.isConnected())
      return;

   auto svp = mTelescope.getSwitch("TELESCOPE_MOTION_WE");
   if (!svp.isValid())
   {
      qDebug() << "Error: unable to find Telescope or TELESCOPE_MOTION_WE switch...";
      return;
   }

   auto motion = svp.findWidgetByName("MOTION_WEST");

   if (speed == SLEW_STOP)
      motion->setState(ISS_OFF);
   else
   {
      setSpeed(speed);
      motion->setState(ISS_ON);
   }

   sendNewProperty(svp);
}

void INDIConnection::setSpeed(int speed)
{
   auto slewRateSP = mTelescope.getSwitch("TELESCOPE_SLEW_RATE");

   if (!slewRateSP.isValid() || speed < 0 || speed > slewRateSP.count())
      return;

   slewRateSP.reset();
   slewRateSP[speed].setState(ISS_ON);
   sendNewProperty(slewRateSP);
}

void INDIConnection::newDevice(INDI::BaseDevice dp)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!dp)
      return;

   QString name(dp.getDeviceName());

   qDebug() << "INDIConnection::newDevice| New Device... " << name;

   mDevices.append(name);
   mTelescope = dp;

   emit newDeviceReceived(name);
}

void INDIConnection::removeDevice(INDI::BaseDevice dp)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (!dp)
      return;

   QString name(dp.getDeviceName());
   int index = mDevices.indexOf(name);
   if (index != -1)
      mDevices.removeAt(index);

   if (mTelescope.isDeviceNameMatch(dp.getDeviceName()))
      mTelescope = INDI::BaseDevice();

   emit removeDeviceReceived(name);
}

void INDIConnection::newProperty(INDI::Property property)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (mTelescope.isDeviceNameMatch(property.getDeviceName()))
      return;

   QString name(property.getName());

   qDebug() << "INDIConnection::newProperty| " << name;

   if (name == "EQUATORIAL_EOD_COORD")
   {
      mCoordinates.RA = property.getNumber()->at(0)->getValue();
      mCoordinates.DEC = property.getNumber()->at(1)->getValue();
   }

   if (!mTelescope.isConnected())
   {
      connectDevice(mTelescope.getDeviceName());
      if (mTelescope.isConnected())
         qDebug() << "connected\n";
   }
}

void INDIConnection::removeProperty(INDI::Property property)
{
   Q_UNUSED(property)
}

void INDIConnection::updateProperty(INDI::Property property)
{
   std::lock_guard<std::mutex> lock(mMutex);
   if (property.isNameMatch("TELESCOPE_SLEW_RATE"))
   {
      auto svp = property.getSwitch();
      emit speedChanged(svp->findOnSwitchIndex());
   }
   else if (property.isNameMatch("EQUATORIAL_EOD_COORD"))
   {
      auto nvp = property.getNumber();
      mCoordinates.RA = nvp->at(0)->getValue();
      mCoordinates.DEC = nvp->at(1)->getValue();
   }
}

void INDIConnection::serverConnected()
{
   std::lock_guard<std::mutex> lock(mMutex);
   emit serverConnectedReceived();
}

void INDIConnection::serverDisconnected(int exit_code)
{
   std::lock_guard<std::mutex> lock(mMutex);
   mDevices.clear();
   emit serverDisconnectedReceived(exit_code);
}

bool INDIConnection::Coordinates::operator==(const INDIConnection::Coordinates &other) const
{
   if (std::abs(RA - other.RA) > std::numeric_limits<double>::epsilon()) return false;
   if (std::abs(DEC - other.DEC) > std::numeric_limits<double>::epsilon()) return false;
   return true;
}

bool INDIConnection::Coordinates::operator!=(const INDIConnection::Coordinates &other) const
{
   return !(*this == other);
}
