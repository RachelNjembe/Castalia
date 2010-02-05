/********************************************************************************
 *  Copyright: National ICT Australia, 2007 - 2010				*
 *  Developed at the Networked Systems theme, ATP lab				*
 *  Author(s): Athanassios Boulis, Dimos Pediaditakis, Yuriy Tselishchev	*
 *  This file is distributed under the terms in the attached LICENSE file.	*
 *  If you do not find this file, copies can be found by writing to:		*
 *										*
 *      NICTA, Locked Bag 9013, Alexandria, NSW 1435, Australia			*
 *      Attention:  License Inquiry.						*
 ********************************************************************************/

#include "RadioModule.h"

Define_Module(RadioModule);

void RadioModule::initialize() {

	// self can be used a a full MAC address
	self = getParentModule()->getParentModule()->getIndex();

	readIniFileParameters();

	changingToState = -1;
	disabled = 0;

	rssiIntegrationTime = symbolsForRSSI * RXmode->bitsPerSymbol / RXmode->datarate;
	timeOfLastSignalChange = 0.0;  // even if left uninitialized, it should not matter.

	TotalPowerReceived_type initialTotalPower;
	initialTotalPower.power_dBm = -200.0;
	initialTotalPower.startTime = 0.0;
	totalPowerReceived.push_front(initialTotalPower);

	CSinterruptMsg = NULL;
	latestCSinterruptTime = 0;

	declareOutput("RX pkt breakdown");
	declareOutput("TXed pkts");
}


void RadioModule::handleMessage(cMessage *msg) {

    if(disabled) {
	delete msg;
	return;
    }

    switch (msg->getKind()) {

	/********************************************************
	 * New signal message from wireless channel.
	 *******************************************************/
	case WC_SIGNAL_START: {

	    WirelessChannelSignalBegin *wcMsg = check_and_cast<WirelessChannelSignalBegin*>(msg);

	    /* If the carrier frequency does not match, it is as if we are not receiving it.
	     * In the future, depending on carrierFreq and bandwidth, we can decide to include
	     * the signal in the received signals list with reduced (spill over) power
	     */
	    if (wcMsg->getCarrierFreq() != carrierFreq) break;

	    collectOutput("RX pkt breakdown", "total");

	    /* if we are not in RX state or we are changing state, then process the
	     * signal minimally. We still need to keep a list of signals because when
	     * we go back in RX we might have some signals active from before, acting
	     * as interference to the new (fully received signals)
	     */
	    if ((state != RX) || (changingToState != -1)) {
		ReceivedSignal_type newSignal;
		newSignal.ID = wcMsg->getNodeID();
		newSignal.power_dBm = wcMsg->getPower_dBm();
		newSignal.bitErrors = ALL_ERRORS;
		receivedSignals.push_front(newSignal);
		collectOutput("RX pkt breakdown", "Failed, non RX state");
		break;  // exit case WC_SIGNAL_START
	    }

	    /* If we are in RX state, go throught the list of received signals and update
	     * bitErrors and currentInterference
	     */
	    list <ReceivedSignal_type>::iterator it1;
	    for (it1 = receivedSignals.begin(); it1 != receivedSignals.end(); it1++) {

		// no need to update bitErrors for an element which will not be received
		if (it1->bitErrors == ALL_ERRORS || it1->bitErrors > maxErrorsAllowed(it1->encoding)) continue;

		// calculate bit errors for the last segment of unchanged signal conditions
		int numOfBits = (int)ceil(RXmode->datarate*SIMTIME_DBL(simTime() - timeOfLastSignalChange));
		double BER = SNR2BER(it1->power_dBm - addPower_dBm(it1->currentInterference, RXmode->noiseFloor));
		it1->bitErrors += bitErrors(BER, numOfBits, maxErrorsAllowed(it1->encoding) - it1->bitErrors);

		// update currentInterference in the received signal structure (*it)
		updateInterference(it1, wcMsg);
	    }

	    //insert new signal in the received signals list,
	    ReceivedSignal_type newSignal;
	    newSignal.ID = wcMsg->getNodeID();
	    newSignal.power_dBm = wcMsg->getPower_dBm();
	    newSignal.modulation = (Modulation_type)wcMsg->getModulationType();
	    newSignal.encoding = (Encoding_type)wcMsg->getEncodingType();
	    newSignal.currentInterference = totalPowerReceived.front().power_dBm;
	    newSignal.maxInterference = newSignal.currentInterference;
	    if ((RXmode->modulation == newSignal.modulation) && (newSignal.power_dBm >= RXmode->sensitivity))
		newSignal.bitErrors = 0;
	    else {
		// ALL_ERRORS signals are kept only for interference and RSSI calculations
		newSignal.bitErrors = ALL_ERRORS ;
		// collect stats
		if (newSignal.power_dBm < RXmode->sensitivity)
		    collectOutput("RX pkt breakdown", "Failed, below sensitivity");
		else
		    collectOutput("RX pkt breakdown", "Failed, wrong modulation");
	    }

	    receivedSignals.push_front(newSignal);
	    updateTotalPowerReceived(newSignal.power_dBm);

	    if ((carrierSenseInterruptEnabled) && (newSignal.power_dBm > CCAthreshold))
	    	updatePossibleCSinterrupt();

	    timeOfLastSignalChange = simTime();
	    
	    break;
	}

	/********************************************************
	 * End signal message from wireless channel.
	 *******************************************************/
	case WC_SIGNAL_END: {
	
	    WirelessChannelSignalEnd *wcMsg = check_and_cast<WirelessChannelSignalEnd*>(msg);
	    int signalID = wcMsg->getNodeID();

	    list <ReceivedSignal_type>::iterator endingSignal;
	    for (endingSignal = receivedSignals.begin(); endingSignal != receivedSignals.end(); endingSignal++) {
		if (endingSignal->ID == signalID) break;
	    }

	    /* We should always find a corresponding received signal in the list
	     */
	    if (endingSignal == receivedSignals.end())
		opp_error("No matching received siganl entry for message WC_SIGNAL_END");

	    /* if we are not in RX state or we are changing state, then just
	     * delete the corresponding signal from the received signals list
	     */
	    if ((state != RX) || (changingToState != -1)) {
	    	receivedSignals.erase(endingSignal);
	    	if (endingSignal->bitErrors != ALL_ERRORS) collectOutput("RX pkt breakdown", "Failed, non RX state");
		break; // exit case WC_SIGNAL_END
	    }

	    /* If we are in RX state, go throught the list of received signals and update
	     * bitErrors and currentInterference, just as we did with start signal.
	     */
	    list <ReceivedSignal_type>::iterator it1;
	    for (it1 = receivedSignals.begin(); it1 != receivedSignals.end(); it1++) {
		// no need to update bitErrors for an element which will not be received
		if (it1->bitErrors == ALL_ERRORS || it1->bitErrors > maxErrorsAllowed(it1->encoding)) continue;

		//calculate bit errors for the last segment of unchanged signal conditions
		int numOfBits = (int)ceil(RXmode->datarate*SIMTIME_DBL(simTime()- timeOfLastSignalChange));
		double BER = SNR2BER(it1->power_dBm - addPower_dBm(it1->currentInterference, RXmode->noiseFloor));
		it1->bitErrors += bitErrors(BER, numOfBits, maxErrorsAllowed(it1->encoding) - it1->bitErrors);

		//update currentInterference in the received signal structure (*it)
		// only if this is NOT the ending signal
		if (it1 != endingSignal) updateInterference(it1, endingSignal);
	    }

	    updateTotalPowerReceived(endingSignal);

	    timeOfLastSignalChange = simTime();

	    // use bit errors and encoding type to determine if the packet is received
	    
	    if (endingSignal->bitErrors != ALL_ERRORS) {
		if (endingSignal->bitErrors <= maxErrorsAllowed(endingSignal->encoding)) {
		    // decapsulate the packet and add the RSSI and LQI fields
		    MacGenericPacket *macPkt = check_and_cast<MacGenericPacket*>(wcMsg->decapsulate());
		    macPkt->getMacInteractionControl().RSSI = readRSSI();
		    macPkt->getMacInteractionControl().LQI = endingSignal->power_dBm - addPower_dBm(endingSignal->maxInterference, RXmode->noiseFloor);
		    send(macPkt,"toMacModule");
		    // collect stats
		    if (endingSignal->maxInterference == -200.0)
			collectOutput("RX pkt breakdown", "Received with NO interference");
		    else
			collectOutput("RX pkt breakdown", "Received despite interference");
		} else {
		    // collect stats
		    if (endingSignal->maxInterference == -200.0)
			collectOutput("RX pkt breakdown", "Failed with NO interference");
		    else
			collectOutput("RX pkt breakdown", "Failed with interference");
		}
	    }

	    receivedSignals.erase(endingSignal);
	    
	    break;
	}

	/********************************************************
	 * Control Command from any layer above
	 *******************************************************/
	case RADIO_CONTROL_COMMAND: {
	RadioControlCommand *radioCmd = check_and_cast<RadioControlCommand*>(msg);

    	switch (radioCmd->getRadioControlCommandKind()) {

	    //case set_encoding++

	    case SET_STATE: {
		/*
		 * The command changes the basic state of the radio. Because of
		 * the transision delay and other restrictions we need to create a
		 * self message and schedule it in the apporpriate time in the future.
		 * Also we need to set changingTostate to the right value. This variable
		 * acts both as a flag that we are in the midst of changing states and
		 * as a place to hold the next state value.
		 */

		// if we are asked to change to the current state, do nothing
		if (state == radioCmd->getState()) break;
		changingToState = radioCmd->getState();

		double transitionDelay = transition[state][changingToState].delay;
		double avgDrawnTransitionPower = transition[state][changingToState].power;

		/* With sleep levels it gets a little more complicated. We can add the trans
		 * delay from going to one sleep level to the other to get the total transDelay,
		 * but the power is not as easy. Ideally we would schedule delayed powerDrawn
		 * messages as we get from one sleep level to the other, but what happens if
		 * we receive another state change message? We would have to cancel these
		 * messages. Instead we are calculating the average power and sending one
		 * powerDrawn message. It might be a little less accurate in rare situations
		 * (very fast state changes), but cleaner in implementation.
		 */
		if (state == SLEEP) {
		    list <SleepLevel_type>::iterator it1 = sleepLevel;
		    while (it1 != sleepLevelList.begin()) {
			double levelDelay = it1->transitionUp.delay;
			double levelPower = it1->transitionUp.power;
			avgDrawnTransitionPower = ((avgDrawnTransitionPower*transitionDelay)+ levelPower*levelDelay)/(transitionDelay+levelDelay);
			transitionDelay += levelDelay;
			it1--;
		    }
		} else if (changingToState == SLEEP) {
		    list <SleepLevel_type>::iterator it1 = sleepLevelList.begin();
		    while (it1 != sleepLevel ) {
			double levelDelay = it1->transitionDown.delay;
			double levelPower = it1->transitionDown.power;
			avgDrawnTransitionPower = ((avgDrawnTransitionPower*transitionDelay)+ levelPower*levelDelay)/(transitionDelay+levelDelay);
			transitionDelay += levelDelay;
			it1++;
		    }
		}

		powerDrawn(avgDrawnTransitionPower);
		trace() << "SET STATE command received, changing state to " << changingToState << " (" <<
			(changingToState == TX ? "TX" : (changingToState == RX ? "RX" : "SLEEP" )) << ")";

		scheduleAt(simTime()+ transitionDelay, new cMessage("Enter Radio state", RADIO_ENTER_STATE));

		break;
	    }

				/*
				 * For the rest of the control commands we do not need to take any special
				 * measures, or create new messages. We just parse the command and assign
				 * the new value to the appropriate variable. We do not need to change the
				 * drawn power, or otherwise change the current behaviour of the radio. If
				 * the radio is transmiting we will continue to TX with the old power until
				 * the buffer is flushed and we try to TX again. If we are sleeping and change
				 * the sleepLevel, the power will change the next time to go to sleep. Only
				 * exception is RX mode where we change the power drawn, even though we keep
				 * receiving currently received signals as with the old mode. We could go and
				 * make all bitErrors = ALL_ERRORS, but not worth the trouble I think.
				 */
    			case SET_MODE: {
					// get the mode name from the command
					string modeName(radioCmd->getName());
					list <RXmode_type>::iterator it1;
					// find the mode in the list of RXmodes and assign it to RXmode
					for (it1 = RXmodeList.begin(); it1 != RXmodeList.end(); it1++) {
						if (modeName.compare(it1->name) == 0) {
							RXmode = it1;
							break;
						}
					}
					if (it1 == RXmodeList.end()) opp_error("Unknown radio RX mode %s",modeName.c_str());

					// update variables depended on RXmode
					rssiIntegrationTime = symbolsForRSSI * RXmode->bitsPerSymbol / RXmode->datarate;

					// if we are in RX state then we should change our drawn power
					if (state == RX) powerDrawn(RXmode->power);
					break;
				}

    			case SET_TX_OUTPUT: {
					double commandPower = radioCmd->getParameter();
					list <TxLevel_type>::iterator it1;
					// find the level in the list of TxLevels
					for (it1 = TxLevelList.begin(); it1 != TxLevelList.end(); it1++) {
						if (it1->txOutputPower == commandPower) {
							TxLevel = it1;
							break;
						}
					}
					if (it1 == TxLevelList.end()) opp_error("Unknown Tx Output Level %f",commandPower);
				}

    			case SET_SLEEP_LEVEL: {
					string sleepLevelName(radioCmd->getName());
					list <SleepLevel_type>::iterator it1;
					for (it1 = sleepLevelList.begin(); it1 != sleepLevelList.end(); it1++) {
						if (sleepLevelName.compare(it1->name) == 0) {
							sleepLevel = it1;
							break;
						}
					}
					if (it1 == sleepLevelList.end()) opp_error("Unknown radio sleep level %s",sleepLevelName.c_str());

					break;
				}

    			case SET_CARRIER_FREQ: {
					carrierFreq = radioCmd->getParameter();
					/* The only measure we take is to clear the receivedSignals list,
					 * as these signals are not valid anymore and in fact could wrongly
					 * create intereference with newly coming signals.
					 */
					receivedSignals.clear();
					break;
				}

				case SET_CCA_THRESHOLD:{
					CCAthreshold = radioCmd->getParameter();
					break;
				}

				case SET_CS_INTERRUPT_ON:{
					carrierSenseInterruptEnabled = true;
					break;
				}
				case SET_CS_INTERRUPT_OFF:{
					carrierSenseInterruptEnabled = false;
					break;
				}
			}
			break;
		}


	    /***********************************************************
	    * Radio self message to complete the transition to a state
	    ***********************************************************/
		case RADIO_ENTER_STATE: {

			state = (BasicState_type)changingToState;

			trace() << "change to " << state << " (" << (state == TX ? "TX" : (state == RX ? "RX" : "SLEEP" )) << ") completes NOW";
			changingToState = -1;

			switch (state) {

				case TX:{
					if (!radioBuffer.empty()) {
						double timeToTxPacket = popAndSendToWirelessChannel();
						powerDrawn(TxLevel->txPowerConsumed);
						// flush the total received power history
						totalPowerReceived.clear();

						// don't like this way too much
						changingToState = TX;
						scheduleAt(simTime() + timeToTxPacket, new cMessage("continueTX", RADIO_ENTER_STATE));
					} else {
						// send a command to change to RX, don't just do it (state = RX;)
						RadioControlCommand *radioCmd = new RadioControlCommand("TX->RX", RADIO_CONTROL_COMMAND);
						radioCmd->setRadioControlCommandKind(SET_STATE);
						radioCmd->setState(RX);
						scheduleAt(simTime(), radioCmd);
					}
					break;
				}
				case RX:{
					powerDrawn(RXmode->power);
					/* Our total received power history was flushed before
					 * we need to recalculated it, adding all currently received signals
					 */
					updateTotalPowerReceived();
					break;
				}
				case SLEEP:{
					powerDrawn(sleepLevel->power);
					// flush the total received power history
					totalPowerReceived.clear();
					break;
				}
			}
			break;
		}


	    /**************************************************************
	    * Packet from MAC level arrived. Buffer it, if there is space
	    **************************************************************/
		case MAC_LAYER_PACKET: {
		    MacGenericPacket *macPkt = check_and_cast<MacGenericPacket*>(msg);

		    int totalSize = macPkt->getByteLength() + PhyFrameOverhead;
		    if (maxPhyFrameSize != 0 && totalSize > maxPhyFrameSize) {
			trace() << "WARNING: MAC sent to Radio an oversized packet ("<<
			    macPkt->getByteLength()+PhyFrameOverhead <<" bytes) packet dropped";
			break;
		    }

		    if((int)radioBuffer.size() < bufferSize) {
			trace() << "Buffered [" << macPkt->getName() << "] from MAC layer";
			radioBuffer.push(macPkt);
			// we use return instead of break that leads to message deletiion at the end
			// to avoid unnecessary message duplication
			return;
		    } else {
			trace() << "WARNING: discarding [" << macPkt->getName() << "] from MAC layer because Radio buffer is full";
			RadioControlMessage *fullBuffMsg = new RadioControlMessage("Radio buffer full", RADIO_CONTROL_MESSAGE);
			fullBuffMsg->setRadioControlMessageKind(RADIO_BUFFER_FULL);
			send(fullBuffMsg, "toMacModule");
		    }
		    break;
		}


       /**************************************************************
	    * Last two kinds of messages disable the radio
	    **************************************************************/
		case OUT_OF_ENERGY: {
			trace() << "Radio disabled: Out of energy";
			disabled = 1;
			break;
		}

		case DESTROY_NODE:	{
			trace() << "Radio disabled: Destroy node";
			disabled = 1;
			break;
		}

		default: {
			opp_error("\n[Radio_%d] t= %f: ERROR: received packet of unknown type.\n", self, SIMTIME_DBL(simTime()));
			break;
		}
	}
	delete msg;
	msg = NULL;		// safeguard
}



void RadioModule::finishSpecific() {
	MacGenericPacket *macPkt;
	while(!radioBuffer.empty()) {
	    macPkt = radioBuffer.front();
	    radioBuffer.pop();
	    cancelAndDelete(macPkt);
	}
	TxLevelList.clear();
	RXmodeList.clear();
	sleepLevelList.clear();
	receivedSignals.clear();
	totalPowerReceived.clear();
}


void RadioModule::readIniFileParameters(void) {
    bufferSize = par("bufferSize");
    maxPhyFrameSize = par("maxPhyFrameSize");
    PhyFrameOverhead = par("phyFrameOverhead");

    symbolsForRSSI = par("symbolsForRSSI");
    parseRadioParameterFile(par("RadioParametersFile"));
    string startingMode = par("mode");

    if (startingMode.compare("") == 0) {
	RXmode = RXmodeList.begin();
    } else {
	//add parsing
    }

    string startingState = par("state");
    state = RX;
    powerDrawn(RXmode->power); //++++ unless other state
    //add parsing

    string startingTxPower = par("TxOutputPower");
    if (startingTxPower.compare("") == 0) {
	TxLevel = TxLevelList.begin();
    } else {
	double startingTxPower_dBm;
	if (parseFloat(startingTxPower.c_str(),&startingTxPower_dBm)) opp_error("Unable to parse TxOutputPower %s", startingTxPower.c_str());
	list <TxLevel_type>::iterator it1;
	for (it1 = TxLevelList.begin(); it1 != TxLevelList.end(); it1++) {
	    if (it1->txOutputPower == startingTxPower_dBm) {
		TxLevel = it1;
		break;
	    }
	}
	if (it1 == TxLevelList.end()) opp_error("Unknown default Tx Output Power Level %f", startingTxPower_dBm);
    }

    carrierFreq = par("carrierFreq");
    collisionModel = (CollisionModel_type)((int)par("collisionModel"));
    CCAthreshold = par("CCAthreshold");
    carrierSenseInterruptEnabled = par("carrierSenseInterruptEnabled");
}


/* Create two wireless channel messages to signify packet transmission
 * and send them txTime apart.
 */
double RadioModule::popAndSendToWirelessChannel() {

    MacGenericPacket *macPkt = radioBuffer.front();
    radioBuffer.pop();

    //Generate begin and end tx messages
    WirelessChannelSignalBegin *begin = new WirelessChannelSignalBegin("WC_BEGIN",WC_SIGNAL_START);
    begin->setNodeID(self);
    begin->setPower_dBm(TxLevel->txOutputPower);
    begin->setCarrierFreq(carrierFreq);
    begin->setBandwidth(RXmode->bandwidth);
    begin->setModulationType(RXmode->modulation);
    begin->setEncodingType(encoding);

    WirelessChannelSignalEnd *end = new WirelessChannelSignalEnd("WC_END",WC_SIGNAL_END);
    end->setByteLength(PhyFrameOverhead);
    end->setNodeID(self);
    end->encapsulate(macPkt);

	//calculate the TX time based on the length of the packet
    double txTime = ((double)(end->getByteLength() * 8.0f)) / RXmode->datarate;

    send(begin, "toCommunicationModule");
    sendDelayed(end, txTime, "toCommunicationModule");

	// keep stats of the packets TXed
	collectOutput("TXed pkts");
    return txTime;
}


/* Update the history of total power received. Overloaded method
 * This version is used when a new signal is added
 */
void RadioModule::updateTotalPowerReceived(double newSignalPower) {

	TotalPowerReceived_type newElement;
	// we are assuming additive power. In reality it is more complex.
	newElement.power_dBm = addPower_dBm(totalPowerReceived.front().power_dBm, newSignalPower);
	newElement.startTime = simTime();

	totalPowerReceived.push_front(newElement);
}

/* Update the history of total power received. Overloaded method
 * This version is used when an old signal ends
 */
void RadioModule::updateTotalPowerReceived(list <ReceivedSignal_type>::iterator endingSignal){

	TotalPowerReceived_type newElement;
	// we are assuming additive power. In reality it is more complex.
	newElement.power_dBm = subtractPower_dBm(totalPowerReceived.front().power_dBm, endingSignal->power_dBm);
	newElement.startTime = simTime();

	totalPowerReceived.push_front(newElement);
}

/* Update the history of total power received. Overloaded method
 * This version is used when we just enter RX state
 */
void RadioModule::updateTotalPowerReceived() {

    TotalPowerReceived_type newElement;
    newElement.power_dBm = -200.0;  // initialize to a value which is practically 0 mW

    /* Go through the list of currently received signals and add up their powers
     * We are assuming additive power. In reality it is more complex.
     * Also do some housekeeping while calculating total power:
     * Signals already active when we just enter RX, cannot be received.
     * We tag them here as such
     */
    list <ReceivedSignal_type>::iterator it1;
    for (it1 = receivedSignals.begin(); it1 != receivedSignals.end(); it1++) {
	newElement.power_dBm = addPower_dBm(it1->power_dBm, newElement.power_dBm);
	if (it1->bitErrors != ALL_ERRORS) {
	    collectOutput("RX pkt breakdown", "Failed, non RX state");
	    it1->bitErrors = ALL_ERRORS;
	}
    }
    newElement.startTime = simTime();
    totalPowerReceived.push_front(newElement);
}

/* Update interference of one element in the receivedSignals list. Overloaded method.
 * This version is used when a new signal is added (WC_SIGNAL_START)
 * Note that the last argument is a message
 */
void RadioModule::updateInterference(list <ReceivedSignal_type>::iterator it1, WirelessChannelSignalBegin *wcMsg) {

    switch (collisionModel) {

	case NO_INTERFERENCE_NO_COLLISIONS:{
	    return;
	}

	case SIMPLE_COLLISION_MODEL: {
	    if (wcMsg->getPower_dBm() > RXmode->sensitivity) {
		it1->bitErrors = maxErrorsAllowed(it1->encoding) + 1; // corrupt the signal
		it1->maxInterference = 0.0;  // a big interference value in dBm
	    }
	    return;
	}

	case ADDITIVE_INTERFERENCE_MODEL:{
	    it1->currentInterference = addPower_dBm(it1->currentInterference,wcMsg->getPower_dBm());
	    if (it1->currentInterference > it1->maxInterference)
		it1->maxInterference = it1->currentInterference;
	    return;
	}

	case COMPLEX_INTERFERENCE_MODEL:{
	    // not implemented yet
	    return;
	}
    }
}

/* Update interference of one element in the receivedSignals list. Overloaded method.
 * This version is used when a new signal is subtracted (WC_SIGNAL_END)
 * note that the last argument is an iterator to a list
 */
void RadioModule::updateInterference(list <ReceivedSignal_type>::iterator it1, list <ReceivedSignal_type>::iterator endingSignal) {

	switch (collisionModel) {
		case NO_INTERFERENCE_NO_COLLISIONS: {
			return;
		}
		case SIMPLE_COLLISION_MODEL: {
			return; // do nothing, this signal corrupted/destroyed other signals already
		}
		case ADDITIVE_INTERFERENCE_MODEL: {
			it1->currentInterference = subtractPower_dBm(it1->currentInterference, endingSignal->power_dBm);
			if (it1->currentInterference > it1->maxInterference) it1->maxInterference = it1->currentInterference;
			return;
		}
		case COMPLEX_INTERFERENCE_MODEL: {
			// not implemented yet
			return;
		}
	}
}

/* Calculate RSSI based on the history of totalReceivedPower
 */
double RadioModule::readRSSI() {

    double rssiEnergy = 0.0;

    // if we are not RXing return the appropriate error code
    if (state != RX) return CS_NOT_VALID;

    simtime_t limitTime = simTime() - rssiIntegrationTime;
    simtime_t currentTime = simTime();

    list <TotalPowerReceived_type>::iterator it1 = totalPowerReceived.begin();
    while ( currentTime > limitTime ){
	rssiEnergy += it1->power_dBm * SIMTIME_DBL(currentTime - max(it1->startTime, limitTime));
	currentTime = it1->startTime;
	it1++;
	// if we have not RXed long enough, then return an error code
	if (it1 == totalPowerReceived.end()) break;
    }
    if (currentTime > limitTime) return CS_NOT_VALID_YET;

    // the rest of the elements are now irrelevant and should be deleted
    totalPowerReceived.erase(it1, totalPowerReceived.end());
    return rssiEnergy/rssiIntegrationTime;
}

/* A method to calculate a possible carrier sense in the future
 * and schedule a message to notify layers above.
 */
void RadioModule::updatePossibleCSinterrupt() {

    if (simTime() < latestCSinterruptTime)
	cancelAndDelete(CSinterruptMsg);

    // initialize RSSI variable to the current rssi reading
    double RSSI = readRSSI();

    // if we are above the threshold, no point in scheduling an interrupt
    if (RSSI > CCAthreshold) return;

    double currentPower = totalPowerReceived.front().power_dBm;
    // The earliest time that we are concerned in our search.
    simtime_t limitTime = simTime() - rssiIntegrationTime;

    // Note: we are iterating in reverse order
    list <TotalPowerReceived_type>::reverse_iterator it1;
    for (it1 = totalPowerReceived.rbegin(); it1 != totalPowerReceived.rend(); it1++) {
	double triggerDiff = CCAthreshold - RSSI;
	list <TotalPowerReceived_type>::reverse_iterator it2 = it1;
	it2++;
	simtime_t nextlimitTime = it2->startTime;

	if (currentPower > it1->power_dBm) {
	    /* We are looking for a time (timeToInterrupt) after limitTime that RSSI will become
	     * greater than CCAthreshold. Thus the following inequality should be satisfied:
	     *((timeToInterrupt - limitTime)/rssiIntegrationTime)*(currentPower - it1->power) >= triggerDiff
	     * solving for timeToInterupt yields:
	     */
	    simtime_t timeToInterrupt = (triggerDiff / (currentPower - it1->power_dBm) * rssiIntegrationTime) + limitTime;
	    
	    /* If the calculation yields a time before the next element of the history of totalPower
	     * begins then we have our new interrupt. Otherwise we have to process the next element
	     */
	    if (timeToInterrupt < nextlimitTime) {
		///schedule message
		CSinterruptMsg = new RadioControlMessage("CS Interrupt", RADIO_CONTROL_MESSAGE);
		CSinterruptMsg->setRadioControlMessageKind(CARRIER_SENSE_INTERRUPT);
		latestCSinterruptTime = simTime() + (timeToInterrupt - limitTime); 
		sendDelayed(CSinterruptMsg, (timeToInterrupt - limitTime), "toMacModule");
		return;
	    }
	}
	RSSI += SIMTIME_DBL((nextlimitTime - limitTime)/rssiIntegrationTime)*(currentPower - it1->power_dBm);
	limitTime = nextlimitTime;
    }
}

/* A method to convert SNR to BER for all the modulation types we support
 */
double RadioModule::SNR2BER(double SNR_dB) {

    switch (RXmode->modulation) {

	case FSK:
	    return 0.5 * exp(((-0.5) * RXmode->noiseBandwidth / RXmode->datarate)* pow(10.0, (SNR_dB/10.0)));

	case PSK:
    	    return 0.5 * erfc(sqrt(pow(10.0,(SNR_dB/10.0)) * RXmode->noiseBandwidth / RXmode->datarate));

	case DIFFBPSK:
	    return 0.5 * exp((RXmode->noiseBandwidth / RXmode->datarate)* pow(10.0, (SNR_dB/10.0)));

	case DIFFQPSK:
	    return diffQPSK_SNR2BER(SNR_dB);

	case CUSTOM: {
	    if (customModulation[0].SNR > SNR_dB) return 0.0;
	    if (customModulation.back().SNR < SNR_dB) return 1.0;
	    for (int i = 0; i < (int)customModulation.size()-1; i++) {
	        if (customModulation[i+1].SNR < SNR_dB) continue;
	        if (customModulation[i+1].SNR - SNR_dB > SNR_dB - customModulation[i].SNR)
		    return customModulation[i].BER;
		else
		    return customModulation[i+1].BER;
	    }
	    // went through the whole array
	    return 1.0;
	}

	case IDEAL:
	    return (SNR_dB > IDEAL_MODULATION_THRESHOLD? 1.0:0.0);

	default:
	    opp_error("Modulation type not defined");
    }
    return 1.0;
}

/* A method to be used by MAC or layers above
 * Despite its name it does not return bool (but CLEAR=1 BUSY=0)
 * since it also has to return some possible codes for non-valid
 */
CCA_result RadioModule::isChannelClear() {

	double value = readRSSI();
	if (value < CS_NOT_VALID) return (value < CCAthreshold)? CLEAR:BUSY;

	/* Otherwise an error has occured and we are returning the error code.
	 * CS_NOT_VALID means that the radio is not in RX, so we have to change
	 * to RX first. CS_NOT_VALID_YET means we are in RX, just not long enough
	 * The only thing we need to do is wait a little more for the CS to be valid.
	 */
	else if (value == CS_NOT_VALID) return CS_NOT_VALID;
	else return CS_NOT_VALID_YET;
}


/* An advanced way of calculating the number of errors by checking if
 * exactly i errors happened, where i = 0..maxallowed.
 */
int RadioModule::bitErrors(double BER, int numOfBits, int maxBitErrorsAllowed){

	double cumulativeProbabilityOfUnrealizedEvents = 0.0;
	int bitErrors;
	for (bitErrors=0; bitErrors <= maxBitErrorsAllowed; bitErrors++)
	{
		double prob = probabilityOfExactly_N_Errors(BER, bitErrors, numOfBits);

		if (genk_dblrand(0) <= prob / (1.0 - cumulativeProbabilityOfUnrealizedEvents)) break;

		cumulativeProbabilityOfUnrealizedEvents += prob;
		if (bitErrors == maxBitErrorsAllowed) {bitErrors++;  break;}
	}
	return bitErrors;
}


void RadioModule::parseRadioParameterFile(const char * fileName) {
    if (strlen(fileName) == 0) opp_error("Radio parameters file not specified");
    ifstream f(fileName);
    if (!f.is_open()) opp_error("Error reading from radio parameters file %s\n", fileName);

    string s; const char *ct;
    map<int,int> visited;
    int section = -1; // -1: ERROR, 1: RX MODES, 2: TX LEVELS, 3: SLEEP LEVELS, 4: DELAY TRANSITION 5: POWER TRANSITION
    while (getline(f,s)) {
	// find and remove comments
	size_t pos = s.find('#');
	if (pos != string::npos) s.replace(s.begin()+pos,s.end(),"");

	// find and remove unneeded spaces
	pos = s.find_last_not_of(" \t\n");
	if (pos != string::npos) {
	    s.erase(pos + 1);
	    pos = s.find_first_not_of(" \t\n");
	    if (pos != string::npos) s.erase(0, pos);
	} else s.erase(s.begin(), s.end());

	if (s.length() == 0) continue;

	if (s.compare("RX MODES") == 0) { visited[section] = 1; section = 1; }
	else if (s.compare("TX LEVELS") == 0) { visited[section] = 1; section = 2; }
	else if (s.compare("SLEEP LEVELS") == 0) { visited[section] = 1; section = 3; }
	else if (s.compare("DELAY TRANSITION MATRIX") == 0) { visited[section] = 1; section = 4; }
	else if (s.compare("POWER TRANSITION MATRIX") == 0) { visited[section] = 1; section = 5; }
	else if (section == -1 || visited[section] == 1) opp_error("Bad syntax of radio parameters file, expecting label:\n%s",ct);
	else {
	    if (section == 1) {
		// parsing lines in the following format:
		// Name, dataRate(kbps), modulationType, bitsPerSymbol, bandwidth(MHz), noiseBandwidth(MHz), noiseFloor(dBm), sensitivity(dBm), powerConsumed(mW)

		RXmode_type rxmode;
		cStringTokenizer t(s.c_str(),", \t"); ct = t.nextToken();	//break the string with ',' delimiter
		if (ct == NULL) opp_error("Bad syntax of radio parameters file, expecting rx mode name:\n%s",ct);
		rxmode.name = string(ct); ct = t.nextToken();

		if (parseFloat(ct,&rxmode.datarate)) opp_error("Bad syntax of radio parameters file, expecting data rate for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		//convert kbps to bps
		rxmode.datarate = rxmode.datarate*1000.0;
		ct = t.nextToken();
		rxmode.modulation = parseModulationType(ct);
		ct = t.nextToken();
		if (parseInt(ct,&rxmode.bitsPerSymbol)) opp_error("Bad syntax of radio parameters file, expecting bits per symbol for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		ct = t.nextToken();
		if (parseFloat(ct,&rxmode.bandwidth)) opp_error("Bad syntax of radio parameters file, expecting bandwidth for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		ct = t.nextToken();
		if (parseFloat(ct,&rxmode.noiseBandwidth)) opp_error("Bad syntax of radio parameters file, expecting noise bandwidth for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		//convert kHz to Hz
		rxmode.noiseBandwidth = rxmode.noiseBandwidth*1000.0; 	
		ct = t.nextToken();
		if (parseFloat(ct,&rxmode.noiseFloor)) opp_error("Bad syntax of radio parameters file, expecting noise floor for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		ct = t.nextToken();
		if (parseFloat(ct,&rxmode.sensitivity)) opp_error("Bad syntax of radio parameters file, expecting sensitivity for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		ct = t.nextToken();
		if (parseFloat(ct,&rxmode.power)) opp_error("Bad syntax of radio parameters file, expecting power for rx mode %s:\n%s",rxmode.name.c_str(),ct);
		ct = t.nextToken();
		if (ct != NULL) opp_error("Bad syntax of radio parameters file, unexpected input for rx mode %s:\n%s",rxmode.name.c_str(),ct);

		list <RXmode_type>::iterator it1;
		for (it1 = RXmodeList.begin(); it1 != RXmodeList.end(); it1++) {
		    if (rxmode.name.compare(it1->name) == 0)
			opp_error("Bad syntax of radio parameters file, duplicate RX mode %s",rxmode.name.c_str());
		}
		RXmodeList.push_front(rxmode);

	    } else if (section == 2) {
		// parsing lines in the following format:
		// Tx_dBm +1, 0, -2, -5, -10, -15
		// Tx_mW 21, 17, 15, 12, 10, 7

		cStringTokenizer t(s.c_str(),", \t"); ct = t.nextToken();
		if (ct == NULL) opp_error("Bad syntax of radio parameters file, expecting Tx_dBm or Tx_mW label");
		int type = 0; //1->dbM, 2->mW

		if (strncmp(ct,"Tx_dBm",strlen("Tx_dBm")) == 0) type = 1;
		else if (strncmp(ct,"Tx_mW",strlen("Tx_mW")) == 0) type = 2;
		else opp_error("Bad syntax of radio parameters file, expecting Tx_dBm or Tx_mW label:\n%s",ct);

		if (TxLevelList.empty()) { // will insert new elements
		    while (ct = t.nextToken()) {
			TxLevel_type txlevel;
			double tmp;
			if (parseFloat(ct,&tmp)) opp_error("Bad syntax of radio parameters file, expecting power value for tx level:\n%s",ct);
			if (type == 1) txlevel.txOutputPower = tmp; else txlevel.txPowerConsumed = tmp;
			TxLevelList.push_back(txlevel);
		    }
		} else { // will modify existing elements
		    list <TxLevel_type>::iterator it1;
		    for (it1 = TxLevelList.begin(); it1 != TxLevelList.end(); it1++) {
			ct = t.nextToken();
			double tmp;
			if (ct == NULL) opp_error("Bad syntax of radio parameters file, expecting number of elements in Tx_dBm list and tx_mW list must be equal");
			if (parseFloat(ct,&tmp)) opp_error("Bad syntax of radio parameters file, expecting power value for tx level:\n%s",ct);
			if (type == 1) it1->txOutputPower = tmp; else it1->txPowerConsumed = tmp;
		    }
		    if (t.nextToken() != NULL) opp_error("Bad syntax of radio parameters file, expecting number of elements in Tx_dBm list and tx_mW list must be equal");
		}

	    } else if (section == 3) {
		// parsing lines in the following format:
		// name, power(mW), delay to switch to mode above(ms), power to switch to mode above (mW), delay to switch to mode below(ms), power to switch to mode below (mW)

		SleepLevel_type sleeplvl;
		cStringTokenizer t(s.c_str(),", \t"); ct = t.nextToken();	//break the string with ',' delimiter
		sleeplvl.name = string(ct); ct = t.nextToken();

		if (parseFloat(ct,&sleeplvl.power)) opp_error("Bad syntax of radio parameters file, expecting power for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		ct = t.nextToken();

		if (sleepLevelList.empty()) { //this is the first element, to txUp should be '-'
		    if (ct[0] != '-' || ct[1]) opp_error("Bad syntax of radio parameters file, expecting '-' as transition up delay for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		    ct = t.nextToken();
		    if (ct[0] != '-' || ct[1]) opp_error("Bad syntax of radio parameters file, expecting '-' as transition up power for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		    sleeplvl.transitionUp.delay = 0.0;
		    sleeplvl.transitionUp.power = 0.0;
		} else { //this is not the first element, so parse txUp
		    if (parseFloat(ct,&sleeplvl.transitionUp.delay)) opp_error("Bad syntax of radio parameters file, expecting transition up delay for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		    ct = t.nextToken();
		    if (parseFloat(ct,&sleeplvl.transitionUp.power)) opp_error("Bad syntax of radio parameters file, expecting transition up power for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		}

		ct = t.nextToken();
		if (ct[0] == '-' && !ct[1]) { // this is the last element
		    visited[section] = 1;
		    ct = t.nextToken();
		    if (ct[0] != '-' || ct[1]) opp_error("Bad syntax of radio parameters file, expecting '-' as transition down power for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		    sleeplvl.transitionDown.delay = 0.0;
		    sleeplvl.transitionDown.power = 0.0;
		} else {
		    if (parseFloat(ct,&sleeplvl.transitionDown.delay)) opp_error("Bad syntax of radio parameters file, expecting transition down delay for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		    ct = t.nextToken();
		    if (parseFloat(ct,&sleeplvl.transitionDown.power)) opp_error("Bad syntax of radio parameters file, expecting transition down power for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		}

		ct = t.nextToken();
		if (ct != NULL) opp_error("Bad syntax of radio parameters file, unexpected input for sleep level %s:\n%s",sleeplvl.name.c_str(),ct);
		sleepLevelList.push_back(sleeplvl);

	    } else if (section == 4 || section == 5) {
		// parsing lines in the following format:
		// [RX|TX|SLEEP] [x|-] [x|-] [x|-]

		cStringTokenizer t(s.c_str()," \t");
		ct = t.nextToken();
		int stateTo;
		if (strncmp(ct,"RX",strlen("RX")) == 0) stateTo = 0;
		else if (strncmp(ct,"TX",strlen("TX")) == 0) stateTo = 1;
		else if (strncmp(ct,"SLEEP",strlen("SLEEP")) == 0) stateTo = 2;
		ct = t.nextToken();
		int stateFrom = 0;

		while (stateFrom < 3) {
		    if (stateTo == stateFrom) {
			if (ct[0] != '-' || ct[1]) opp_error("Bad syntax of radio parameters file, expecting '-' as state transition value from %i to %i:\n%s",stateFrom,stateTo,ct);
		    } else {
			double tmp;
			if (parseFloat(ct,&tmp)) opp_error("Bad syntax of radio parameters file, expecting state transition value from %i to %i:\n%s",stateFrom,stateTo,ct);
			if (section == 4) transition[stateFrom][stateTo].delay = tmp;
			else transition[stateFrom][stateTo].power = tmp;
		    }
		    stateFrom++;
		    ct = t.nextToken();
		}
		if (ct != NULL) opp_error("Bad syntax of radio parameters file, unexpected input for transition matrix:\n%s",ct);
	    }
	}
    }
}

Modulation_type RadioModule::parseModulationType(const char *c) {
    string modulation(c);
    if (modulation.compare("CUSTOM") == 0) return CUSTOM;
    if (modulation.compare("IDEAL") == 0) return IDEAL;
    if (modulation.compare("FSK") == 0) return FSK;
    if (modulation.compare("PSK") == 0) return PSK;
    if (modulation.compare("DIFFBPSK") == 0) return DIFFBPSK;
    if (modulation.compare("DIFFQPSK") == 0) return DIFFQPSK;
    return CUSTOM;
}

//wrapper function for atoi(...) call. returns 1 on error, 0 on success
int RadioModule::parseInt(const char * c, int * dst) {
    while (c[0] && (c[0] == ' ' || c[0] == '\t')) c++;
    if (!c[0] || c[0] < '0' || c[0] > '9') return 1;
    *dst = atoi(c);
    return 0;
}

//wrapper function for strtof(...) call. returns 1 on error, 0 on success
int RadioModule::parseFloat(const char * c, double * dst) {
    char * tmp;
    *dst = strtof(c,&tmp);
    if (c == tmp) return 1;
    return 0;
}

void RadioModule::ReceivedSignalDebug(const char * description) {
    list <ReceivedSignal_type>::iterator it1;
    trace() << "*** RECEIVED SIGNALS LIST DEBUG AT " << description << " ***";
    for (it1 = receivedSignals.begin(); it1 != receivedSignals.end(); it1++) {
	trace() << "ID:" << it1->ID << ", power:" << it1->power_dBm << ", crIntrf:" << it1->currentInterference << ", bitErr:" << it1->bitErrors;
    }
}
