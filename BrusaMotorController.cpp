/*
 * BrusaMotorController.cpp
 *
 *
 Copyright (c) 2013 Collin Kidder, Michael Neuweiler, Charles Galpin

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include "config.h"
#ifdef CFG_ENABLE_DEVICE_MOTORCTRL_BRUSA_DMC5
#include "BrusaMotorController.h"

/*
 Warning:
 At high speed disable the DMC_EnableRq can be dangerous because a field weakening current is
 needed to achieve zero torque. Switching off the DMC in such a situation will make heavy regenerat-
 ing torque that can't be controlled.
 */

BrusaMotorController::BrusaMotorController() {
	dmcReady = false;
	dmcRunning = false;
	dmcError = false;
	dmcWarning = false;
	torqueAvailable = 0;
	torqueActual = 0;
	speedActual = 0;

	dcVoltage = 0;
	dcCurrent = 0;
	acCurrent = 0;
	mechanicalPower = 0;

	temperatureInverter = 0;
	temperatureMotor = 0;
	temperatureSystem = 0;

	errorBitField = 0;
	warningBitField = 0;
	statusBitField = 0;

	maxPositiveTorque = 0;
	minNegativeTorque = 0;
	limiterStateNumber = 0;

	tickCounter = 0;
	powerMode = modeTorque;

	maxTorque = 20; // TODO: only for testing, in tenths Nm, so 2Nm max torque, remove for production use
	maxRPM = 2000; // TODO: only for testing, remove for production use
	requestedRPM = 0;
	requestedTorque = 0;
	requestedThrottle = 0;
}

void BrusaMotorController::setup() {
	TickHandler::getInstance()->detach(this);
	MotorController::setup(); // run the parent class version of this function

	// register ourselves as observer of 0x258-0x268 and 0x458 can frames
	CanHandler::getInstanceEV()->attach(this, CAN_MASKED_ID_1, CAN_MASK_1, false);
	CanHandler::getInstanceEV()->attach(this, CAN_MASKED_ID_2, CAN_MASK_2, false);

	TickHandler::getInstance()->attach(this, CFG_TICK_INTERVAL_MOTOR_CONTROLLER_BRUSA);
}

void BrusaMotorController::handleTick() {
	tickCounter++;

	sendControl();	// send CTRL every 20ms : 20 00 2E E0 00 00 00 00
	if (tickCounter > 4) {
		sendControl2();	// send CTRL_2 every 100ms : 00 00 00 00 00 00 00 00
		sendLimits();	// send LIMIT every 100ms : 0D 70 11 C6 00 00 00 00
		tickCounter = 0;
	}
}

void BrusaMotorController::sendControl() {
	prepareOutputFrame(CAN_ID_CONTROL);


	//TODO: remove ramp testing
	requestedTorque = 50;
	if (speedActual == 0)
		requestedRPM = 1000;
	if (speedActual > 950)
		requestedRPM = 0;



	outputFrame.data[0] = enablePositiveTorqueSpeed | enableNegativeTorqueSpeed;
	if (dmcError) {
		outputFrame.data[0] |= clearErrorLatch;
	} else {
		if (dmcReady || speedActual > 1000) { // see warning about field weakening current to prevent uncontrollable regen
			outputFrame.data[0] |= enablePowerStage;
			if (dmcRunning) {
				// outputFrame.data[0] |= enableOscillationLimiter;
				if (powerMode == modeSpeed)
					outputFrame.data[0] |= enableSpeedMode;

				// TODO: differ between torque/speed mode
				// TODO: check for maxRPM and maxTorque

				// set the speed in rpm
				outputFrame.data[2] = (requestedRPM & 0xFF00) >> 8;
				outputFrame.data[3] = (requestedRPM & 0x00FF);

				// set the torque in 0.01Nm (GEVCU uses 0.1Nm -> multiply by 10)
				outputFrame.data[4] = ((requestedTorque * 10) & 0xFF00) >> 8;
				outputFrame.data[5] = ((requestedTorque * 10) & 0x00FF);
			}
		}
	}

	CanHandler::getInstanceEV()->sendFrame(outputFrame);
}

void BrusaMotorController::sendControl2() {
	//TODO: move variables + initialization
	uint16_t torqueSlewRate = 0; // for torque mode only: slew rate of torque value, 0=disabled, in 0.01Nm/sec
	uint16_t speedSlewRate = 0; //  for speed mode only: slew rate of speed value, 0=disabled, in rpm/sec
	uint16_t maxMechanicalPowerMotor = 50000; // maximal mechanical power of motor in 4W steps
	uint16_t maxMechanicalPowerRegen = 50000; // maximal mechanical power of regen in 4W steps

	prepareOutputFrame(CAN_ID_CONTROL_2);
	outputFrame.data[0] = (torqueSlewRate & 0xFF00) >> 8;
	outputFrame.data[1] = (torqueSlewRate & 0x00FF);
	outputFrame.data[2] = (speedSlewRate & 0xFF00) >> 8;
	outputFrame.data[3] = (speedSlewRate & 0x00FF);
	outputFrame.data[4] = (maxMechanicalPowerMotor & 0xFF00) >> 8;
	outputFrame.data[5] = (maxMechanicalPowerMotor & 0x00FF);
	outputFrame.data[6] = (maxMechanicalPowerRegen & 0xFF00) >> 8;
	outputFrame.data[7] = (maxMechanicalPowerRegen & 0x00FF);

	CanHandler::getInstanceEV()->sendFrame(outputFrame);
}

void BrusaMotorController::sendLimits() {
	//TODO: move variables + initialization
	uint16_t dcVoltLimitMotor = 1000; // minimum DC voltage limit for motoring in 0.1V
	uint16_t dcVoltLimitRegen = 1000; //  maximum DC voltage limit for regen in 0.1V
	uint16_t dcCurrentLimitMotor = 0; // current limit for motoring in 0.1A
	uint16_t dcCurrentLimitRegen = 0; // current limit for regen in 0.1A

	prepareOutputFrame(CAN_ID_LIMIT);
	outputFrame.data[0] = (dcVoltLimitMotor & 0xFF00) >> 8;
	outputFrame.data[1] = (dcVoltLimitMotor & 0x00FF);
	outputFrame.data[2] = (dcVoltLimitRegen & 0xFF00) >> 8;
	outputFrame.data[3] = (dcVoltLimitRegen & 0x00FF);
	outputFrame.data[4] = (dcCurrentLimitMotor & 0xFF00) >> 8;
	outputFrame.data[5] = (dcCurrentLimitMotor & 0x00FF);
	outputFrame.data[6] = (dcCurrentLimitRegen & 0xFF00) >> 8;
	outputFrame.data[7] = (dcCurrentLimitRegen & 0x00FF);

	CanHandler::getInstanceEV()->sendFrame(outputFrame);
}

void BrusaMotorController::prepareOutputFrame(uint32_t id) {
	outputFrame.dlc = 8;
	outputFrame.id = id;
	outputFrame.ide = 0;
	outputFrame.rtr = 0;

	outputFrame.data[1] = 0;
	outputFrame.data[2] = 0;
	outputFrame.data[3] = 0;
	outputFrame.data[4] = 0;
	outputFrame.data[5] = 0;
	outputFrame.data[6] = 0;
	outputFrame.data[7] = 0;
}

void BrusaMotorController::handleCanFrame(RX_CAN_FRAME* frame) {
	switch (frame->id) {
	case CAN_ID_STATUS:
		statusBitField = frame->data[1] | (frame->data[0] << 8);
		torqueAvailable = frame->data[3] | (frame->data[2] << 8);
		torqueActual = frame->data[5] | (frame->data[4] << 8);
		speedActual = frame->data[7] | (frame->data[6] << 8);
		Logger::debug("status: %X, torque avail: %fNm, actual torque: %fNm, speed actual: %drpm", statusBitField, (float)torqueAvailable/100.0F, (float)torqueActual/100.0F, speedActual);

		dmcReady = (statusBitField & stateReady) != 0 ? true : false;
		if (dmcReady)
			Logger::info("DMC5: ready");

		dmcRunning = (statusBitField & stateRunning) != 0 ? true : false;
		if (dmcRunning)
			Logger::info("DMC5: running");

		dmcError = (statusBitField & errorFlag) != 0 ? true : false;
		if (dmcError)
			Logger::error("DMC5: error is present, see error message");

		dmcWarning = (statusBitField & warningFlag) != 0 ? true : false;
		if (dmcWarning)
			Logger::warn("DMC5: warning is present, see warning message");

		if (statusBitField & motorModelLimitation)
			Logger::info("DMC5: torque limit by motor model");
		if (statusBitField & mechanicalPowerLimitation)
			Logger::info("DMC5: torque limit by mechanical power");
		if (statusBitField & maxTorqueLimitation)
			Logger::info("DMC5: torque limit by max torque");
		if (statusBitField & acCurrentLimitation)
			Logger::info("DMC5: torque limit by AC current");
		if (statusBitField & temperatureLimitation)
			Logger::warn("DMC5: torque limit by temperature");
		if (statusBitField & speedLimitation)
			Logger::info("DMC5: torque limit by speed");
		if (statusBitField & voltageLimitation)
			Logger::info("DMC5: torque limit by DC voltage");
		if (statusBitField & currentLimitation)
			Logger::info("DMC5: torque limit by DC current");
		if (statusBitField & torqueLimitation)
			Logger::info("DMC5: torque limitation is active");
		if (statusBitField & slewRateLimitation)
			Logger::info("DMC5: torque limit by slew rate");
		if (statusBitField & motorTemperatureLimitation)
			Logger::warn("DMC5: torque limit by motor temperature");
		break;

	case CAN_ID_ACTUAL_VALUES:
		dcVoltage = frame->data[1] | (frame->data[0] << 8);
		dcCurrent = frame->data[3] | (frame->data[2] << 8);
		acCurrent = frame->data[5] | (frame->data[4] << 8);
		mechanicalPower = frame->data[7] | (frame->data[6] << 8);
		Logger::debug("actual values: DC Volts: %fV, DC current: %fA, AC current: %fA, mechPower: %fkW", (float)dcVoltage / 10.0F, (float)dcCurrent / 10.0F, (float)acCurrent / 4.0F, (float)mechanicalPower / 62.5F);
		break;

	case CAN_ID_ERRORS:
		errorBitField = frame->data[1] | (frame->data[0] << 8) | (frame->data[5] << 16) | (frame->data[4] << 24);
		warningBitField = frame->data[7] | (frame->data[6] << 8);
		Logger::debug("errors: %X, warning: %X", errorBitField, warningBitField);

		//TODO: DMC_CompatibilityWarnings not evaluated at this point. check if needed

		// errors
		if (errorBitField & speedSensorSupply)
			Logger::error("DMC5: speed sensor supply");
		if (errorBitField & speedSensor)
			Logger::error("DMC5: speed sensor");
		if (errorBitField & canLimitMessageInvalid)
			Logger::error("DMC5: can limit message invalid");
		if (errorBitField & canControlMessageInvalid)
			Logger::error("DMC5: can control message invalid");
		if (errorBitField & canLimitMessageLost)
			Logger::error("DMC5: can limit message lost");
		if (errorBitField & overvoltageSkyConverter)
			Logger::error("DMC5: overvoltage sky converter");
		if (errorBitField & voltageMeasurement)
			Logger::error("DMC5: voltage measurement");
		if (errorBitField & shortCircuit)
			Logger::error("DMC5: short circuit");
		if (errorBitField & canControlMessageLost)
			Logger::error("DMC5: can control message lost");
		if (errorBitField & overtemp)
			Logger::error("DMC5: overtemp");
		if (errorBitField & overtempMotor)
			Logger::error("DMC5: overtemp motor");
		if (errorBitField & overspeed)
			Logger::error("DMC5: overspeed");
		if (errorBitField & undervoltage)
			Logger::error("DMC5: undervoltage");
		if (errorBitField & overvoltage)
			Logger::error("DMC5: overvoltage");
		if (errorBitField & overcurrent)
			Logger::error("DMC5: overcurrent");
		if (errorBitField & initalisation)
			Logger::error("DMC5: initalisation");
		if (errorBitField & analogInput)
			Logger::error("DMC5: analogInput");
		if (errorBitField & driverShutdown)
			Logger::error("DMC5: driver shutdown");
		if (errorBitField & powerMismatch)
			Logger::error("DMC5: power mismatch");
		if (errorBitField & canControl2MessageLost)
			Logger::error("DMC5: can Control2 message lost");
		if (errorBitField & motorEeprom)
			Logger::error("DMC5: motor Eeprom");
		if (errorBitField & storage)
			Logger::error("DMC5: storage");
		if (errorBitField & enablePinSignalLost)
			Logger::error("DMC5: lost signal on enable pin");
		if (errorBitField & canCommunicationStartup)
			Logger::error("DMC5: can communication startup");
		if (errorBitField & internalSupply)
			Logger::error("DMC5: internal supply");
		if (errorBitField & acOvercurrent)
			Logger::error("DMC5: AC Overcurrent");
		if (errorBitField & osTrap)
			Logger::error("DMC5: OS trap");

		// warnings
		if (warningBitField & systemCheckActive)
			Logger::warn("DMC5: system check active");
		if (warningBitField & externalShutdownPathAw2Off)
			Logger::warn("DMC5: external shutdown path Aw2 off");
		if (warningBitField & externalShutdownPathAw1Off)
			Logger::warn("DMC5: external shutdown path Aw1 off");
		if (warningBitField & oscillationLimitControllerActive)
			Logger::warn("DMC5: oscillation limit controller active");
		if (warningBitField & driverShutdownPathActive)
			Logger::warn("DMC5: driver shutdown path active");
		if (warningBitField & powerMismatchDetected)
			Logger::warn("DMC5: power mismatch detected");
		if (warningBitField & speedSensorSignal)
			Logger::warn("DMC5: speed sensor signal");
		if (warningBitField & hvUndervoltage)
			Logger::warn("DMC5: HV undervoltage");
		if (warningBitField & maximumModulationLimiter)
			Logger::warn("DMC5: maximum modulation limiter");
		if (warningBitField & temperatureSensor)
			Logger::warn("DMC5: temperature sensor");
		break;

	case CAN_ID_TORQUE_LIMIT:
		maxPositiveTorque = frame->data[1] | (frame->data[0] << 8);
		minNegativeTorque = frame->data[3] | (frame->data[2] << 8);
		limiterStateNumber = frame->data[4];

		Logger::debug("torque limit: max positive: %fNm, min negative: %fNm", (float) maxPositiveTorque / 100.0F, (float) minNegativeTorque / 100.0F, limiterStateNumber);
		break;

	case CAN_ID_TEMP:
		temperatureInverter = frame->data[1] | (frame->data[0] << 8);
		temperatureMotor = frame->data[3] | (frame->data[2] << 8);
		temperatureSystem = frame->data[4];

		Logger::debug("temperature: inverter: %f�C, motor: %f�C, system: %d�C", (float)temperatureInverter / 2.0F, (float)temperatureMotor / 2.0F, temperatureSystem - 50);
		break;

	default:
		Logger::debug("DMC5: received unknown frame id %X", frame->id);
	}
}

DeviceId BrusaMotorController::getId() {
	return BRUSA_DMC5;
}

#endif
