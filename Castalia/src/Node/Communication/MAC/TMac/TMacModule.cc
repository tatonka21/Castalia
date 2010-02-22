/***************************************************************************
 *  Copyright: National ICT Australia,  2009
 *  Developed at the ATP lab, Networked Systems theme
 *  Author(s): Athanassios Boulis, Yuri Tselishchev
 *  This file is distributed under the terms in the attached LICENSE file.
 *  If you do not find this file, copies can be found by writing to:
 *
 *      NICTA, Locked Bag 9013, Alexandria, NSW 1435, Australia
 *      Attention:  License Inquiry.
 ***************************************************************************/

#include <cmath>
#include "TMacModule.h"

#define BROADCAST_ADDR -1
#define NAV_EXTENSION 		0.0001
#define TX_TIME(x)		(phyLayerOverhead + x)*1/(1000*phyDataRate/8.0)		//x are in BYTES

Define_Module(TMacModule);

void TMacModule::startup() {
    printStateTransitions = par("printStateTransitions");
    ackPacketSize = par("ackPacketSize");
    frameTime = ((double)par("frameTime"))/1000.0;		// just convert msecs to secs
    syncPacketSize = par("syncPacketSize");
    rtsPacketSize = par("rtsPacketSize");
    ctsPacketSize = par("ctsPacketSize");
    resyncTime =  par("resyncTime");
    allowSinkSync = par("allowSinkSync");
    contentionPeriod = ((double)par("contentionPeriod"))/1000.0;		// just convert msecs to secs
    listenTimeout = ((double)par("listenTimeout"))/1000.0;		// TA: just convert msecs to secs (15ms default);
    waitTimeout = ((double)par("waitTimeout"))/1000.0;
    useFRTS = par("useFrts");
    useRtsCts = par("useRtsCts");
    maxTxRetries = par("maxTxRetries");
    
    disableTAextension = par("disableTAextension");
    conservativeTA = par("conservativeTA");
    collisionResolution = par("collisionResolution");
    if (collisionResolution != 2 && collisionResolution != 1 && collisionResolution != 0) {
	trace() << "Unknown value for parameter 'collisionResolution', will default to 1";
	collisionResolution = 1;
    }

    //Initialise state descriptions used in debug output
    if (printStateTransitions) {
        stateDescr[100] = "MAC_STATE_SETUP";
        stateDescr[101] = "MAC_STATE_SLEEP";
        stateDescr[102] = "MAC_STATE_ACTIVE";
        stateDescr[103] = "MAC_STATE_ACTIVE_SILENT";
        stateDescr[104] = "MAC_STATE_IN_TX";
        stateDescr[110] = "MAC_CARRIER_SENSE_FOR_TX_RTS";
        stateDescr[111] = "MAC_CARRIER_SENSE_FOR_TX_DATA";
        stateDescr[112] = "MAC_CARRIER_SENSE_FOR_TX_CTS";
        stateDescr[113] = "MAC_CARRIER_SENSE_FOR_TX_ACK";
        stateDescr[114] = "MAC_CARRIER_SENSE_FOR_TX_SYNC";
        stateDescr[115] = "MAC_CARRIER_SENSE_BEFORE_SLEEP";
        stateDescr[120] = "MAC_STATE_WAIT_FOR_DATA";
        stateDescr[121] = "MAC_STATE_WAIT_FOR_CTS";
        stateDescr[122] = "MAC_STATE_WAIT_FOR_ACK";
    }

    phyDataRate = par("phyDataRate");
    phyDelayForValidCS = (double)par("phyDelayForValidCS")/1000.0; //parameter given in ms in the omnetpp.ini
    phyLayerOverhead = par("phyFrameOverhead");

    //try to obtain the value of isSink parameter from application module
    if(getParentModule()->getParentModule()->findSubmodule("nodeApplication") != -1) {
        cModule *tmpApplication = getParentModule()->getParentModule()->getSubmodule("nodeApplication");
        isSink = tmpApplication->hasPar("isSink") ? tmpApplication->par("isSink") : false;
    } else {
        isSink = false;
    }

    syncPacket = NULL;
    rtsPacket = NULL;
    ackPacket = NULL;
    ctsPacket = NULL;

    macState = MAC_STATE_SETUP;
    scheduleTable.clear();
    primaryWakeup = true;
    needResync = 0;
    currentFrameStart = -1;
    activationTimeout = 0;

    declareOutput("Sent packets breakdown");

    if (isSink && allowSinkSync) createPrimarySchedule();
    else setTimer(SYNC_SETUP,allowSinkSync ? 2*frameTime : 0);
}

void TMacModule::timerFiredCallback(int timer) {
    switch (timer) {

	case SYNC_SETUP: {
	    /* Timeout to hear a schedule packet has expired at this stage, 
	     * MAC is able to create its own schedule after a random offset
	     * within the duration of 1 frame
	     */
	    if (macState == MAC_STATE_SETUP) setTimer(SYNC_CREATE,genk_dblrand(1)*frameTime);
	    break;
	}

	case SYNC_CREATE: {
	    /* Random offset selected for creating a new schedule has expired. 
	     * If at this stage still no schedule was received, MAC creates 
	     * its own schedule and tries to broadcast it
	     */
	    if (macState == MAC_STATE_SETUP) createPrimarySchedule();
	    break;
	}
	
	case SYNC_RENEW: {
	    /* This node is the author of its own primary schedule
	     * It is required to rebroadcast a SYNC packet and also 
	     * schedule a self message for the next RESYNC procedure.
	     */
	    trace() << "Initiated RESYNC procedure";
	    scheduleTable[0].SN++;
	    needResync = 1;
	    setTimer(SYNC_RENEW,resyncTime);
	    break;
	}
	
	case FRAME_START: {
	    /* primaryWakeup variable is used to distinguish between primary and secondary schedules
	     * since the primary schedule is always the one in the beginning of the frame, we set
	     * primaryWakeup to true here.
	     */
	    primaryWakeup = true;

	    // record the current time and extend activation timeout
	    currentFrameStart = activationTimeout = getClock();
	    extendActivePeriod();

	    // schedule the message to start the next frame. Also check for frame offsets
	    // (if we received a RESYNC packet, frame start time could had been shifted due to
	    // clock drift - in this case it is necessary to rebroadcast this resync further)
	    setTimer(FRAME_START,frameTime);
	    if (scheduleTable[0].offset != 0) {
	        trace() << "New frame started, shifted by " << scheduleTable[0].offset;
		scheduleTable[0].offset = 0;
		needResync = 1;
	    } else {
		trace() << "New frame started";
	    }

	    // schedule wakeup messages for secondary schedules within the current frame only
	    for (int i = 1; i < (int)scheduleTable.size(); i++) {
		if (scheduleTable[i].offset < 0) { scheduleTable[i].offset += frameTime; }
		setTimer(WAKEUP_SILENT+i,scheduleTable[i].offset);
	    }

	    // finally, if we were sleeping, need to wake up the radio. And reset the internal MAC
	    // state (to start contending for transmissions if needed)
	    if (macState == MAC_STATE_SLEEP) toRadioLayer(createRadioCommand(SET_STATE,RX));
	    if (macState == MAC_STATE_SLEEP || macState == MAC_STATE_ACTIVE ||
		    macState == MAC_STATE_ACTIVE_SILENT) {
		resetDefaultState();
	    }
	    break;
	}
	
	case CHECK_TA: {
	    /* It is necessary to check activation timeout.
	     * We may need to extend the timeout here based on the current MAC state, or
	     * if timeout expired and we have no reason to extend it, then we need to go to sleep.
	     */
	    if (activationTimeout <= getClock()) {
		
		//if disableTAextension is on, then we will behave as SMAC - simply go to sleep if the active period is over
		if (disableTAextension) {
		    primaryWakeup = false;
		    // update MAC and RADIO states
		    toRadioLayer(createRadioCommand(SET_STATE,SLEEP));
		    setMacState(MAC_STATE_SLEEP);
		}
		
		//otherwise, check MAC state and extend active period or go to sleep
		else if (conservativeTA) {
		    if (macState != MAC_STATE_ACTIVE && macState != MAC_STATE_ACTIVE_SILENT && macState != MAC_STATE_SLEEP) {
			extendActivePeriod();
		    } else {
			performCarrierSense(MAC_CARRIER_SENSE_BEFORE_SLEEP);
		    }
		}
		
		else {
		    primaryWakeup = false;
		    // update MAC and RADIO states
		    toRadioLayer(createRadioCommand(SET_STATE,SLEEP));
		    setMacState(MAC_STATE_SLEEP);
		} 
	    }
	    break;
	}
	
	case CARRIER_SENSE: {
	    /* First it is important to check for valid MAC state
	     * If we heard something on the radio while waiting to start carrier sense,
	     * then MAC was set to MAC_STATE_ACTIVE_SILENT. In this case we can not transmit
	     * and there is no point to perform carrier sense
	     */
	    if(macState == MAC_STATE_ACTIVE_SILENT || macState == MAC_STATE_SLEEP) break;

	    // At this stage MAC can only be in one of the states MAC_CARRIER_SENSE_...
	    if(macState != MAC_CARRIER_SENSE_FOR_TX_RTS && macState != MAC_CARRIER_SENSE_FOR_TX_CTS &&
		    macState != MAC_CARRIER_SENSE_FOR_TX_SYNC && macState != MAC_CARRIER_SENSE_FOR_TX_DATA &&
		    macState != MAC_CARRIER_SENSE_FOR_TX_ACK && macState != MAC_CARRIER_SENSE_BEFORE_SLEEP) {
		trace() << "WARNING: bad MAC state for MAC_SELF_PERFORM_CARRIER_SENSE";
		break;
	    }
	    carrierSenseCallback(radioModule->isChannelClear());
	    break;
	}
	
	case TRANSMISSION_TIMEOUT: {
	    resetDefaultState();
	    break;
	}
	
	case WAKEUP_SILENT: {
	    /* This is the wakeup timer for secondary schedules.
	     * here we only wake up the radio and extend activation timeout for listening.
	     * NOTE that default state for secondary schedules is MAC_STATE_ACTIVE_SILENT
	     */
	
	    activationTimeout = getClock();
	    extendActivePeriod();
	    if(macState == MAC_STATE_SLEEP) {
		toRadioLayer(createRadioCommand(SET_STATE,RX));
		resetDefaultState();
	    }
	    break;
	}
	

	default: {
	    int tmpTimer = timer - WAKEUP_SILENT;
	    if (tmpTimer > 0 && tmpTimer < (int)scheduleTable.size()) {
	        activationTimeout = getClock();
		extendActivePeriod();
		if(macState == MAC_STATE_SLEEP) {
		    toRadioLayer(createRadioCommand(SET_STATE,RX));
		    resetDefaultState();
		}
	    } else {
	        trace() << "Unknown timer " << timer;
	    }
	}
    }
}

void TMacModule::carrierSenseCallback(int returnCode) {
    switch (returnCode) {
	
	case CLEAR: {
	    carrierIsClear();
	    break;
	}
	
	case BUSY: {
	    /* Since we are hearing some communication on the radio we need to do two things:
	     * 1 - extend our active period
	     * 2 - set MAC state to MAC_STATE_ACTIVE_SILENT unless we are actually expecting to receive
	     *     something (or sleeping)
	     */
	    if (macState == MAC_STATE_SETUP || macState == MAC_STATE_SLEEP) break;
	    if (!disableTAextension) extendActivePeriod();
	    if (collisionResolution == 0) {
		if (macState == MAC_CARRIER_SENSE_FOR_TX_RTS || macState == MAC_CARRIER_SENSE_FOR_TX_DATA ||
		    macState == MAC_CARRIER_SENSE_FOR_TX_CTS || macState == MAC_CARRIER_SENSE_FOR_TX_ACK ||
		    macState == MAC_CARRIER_SENSE_FOR_TX_SYNC || macState == MAC_CARRIER_SENSE_BEFORE_SLEEP) {
			resetDefaultState();
		}
	    } else {
		if (macState != MAC_STATE_WAIT_FOR_ACK && macState != MAC_STATE_WAIT_FOR_DATA &&
		    macState != MAC_STATE_WAIT_FOR_CTS) 
			setMacState(MAC_STATE_ACTIVE_SILENT);
	    }
	    break;
	}
	
	case CS_NOT_VALID_YET: {
	    setTimer(CARRIER_SENSE,phyDelayForValidCS);
	    break;
	}
	
	case CS_NOT_VALID: {
	    if (macState != MAC_STATE_SLEEP) {
		toRadioLayer(createRadioCommand(SET_STATE,RX));
		setTimer(CARRIER_SENSE,phyDelayForValidCS);
	    }
	    break;
	}
    }
}

void TMacModule::fromNetworkLayer(cPacket* netPkt, int destination) {
    // Create a new MAC frame from the received packet and buffer it (if possible)
    TMacPacket *macPkt = new TMacPacket("TMAC data packet",MAC_LAYER_PACKET);
    macPkt->setType(DATA_FRAME);
    macPkt->setSource(SELF_MAC_ADDRESS);
    macPkt->setDestination(destination);
    macPkt->setSequenceNumber(txSequenceNum);
    encapsulatePacket(macPkt,netPkt);
    if (bufferPacket(macPkt)) {	// this is causing problems
	if (TXBuffer.size() == 1) checkTxBuffer();
    } else {
	// cancelAndDelete(macPkt);
	//We could send a control message to upper layer to inform of full buffer
    }
}

void TMacModule::finishSpecific() {

    if (packetsSent.size() > 0) {
	trace() << "Sent packets breakdown: ";
	int total = 0;
	for (map<string,int>::iterator i = packetsSent.begin();
	    i != packetsSent.end(); i++) {
	    trace() << i->first << ": " << i->second;
	    total += i->second;
	}
	trace() << "Total: " << total << "\n";
    }
}


/* This function will reset the internal MAC state in the following way:
 * 1 -  Check if MAC is still in its active time, if timeout expired - go to sleep.
 * 2 -  Check if MAC is in the primary schedule wakeup (if so, MAC is able to start transmissions
 *	of either SYNC, RTS or DATA packets after a random contention offset.
 * 3 -  IF this is not primary wakeup, MAC can only listen, thus set state to MAC_STATE_ACTIVE_SILENT
 */
void TMacModule::resetDefaultState()  {
    if (activationTimeout <= getClock()) {
	performCarrierSense(MAC_CARRIER_SENSE_BEFORE_SLEEP);
    } else if (primaryWakeup) {
	simtime_t randomContentionInterval = genk_dblrand(1)*contentionPeriod;
	if (needResync) {
	    if (syncPacket) cancelAndDelete(syncPacket);
	    syncPacket = new TMacPacket("TMAC SYNC packet", MAC_LAYER_PACKET);
	    syncPacket->setSyncId(SELF_MAC_ADDRESS);
	    syncPacket->setSequenceNumber(scheduleTable[0].SN);
	    syncPacket->setSync(currentFrameStart + frameTime - getClock() - TX_TIME(syncPacketSize) - randomContentionInterval);
	    syncPacket->setByteLength(syncPacketSize);
	    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_SYNC,randomContentionInterval);
	    return;
	}
	
	while (!TXBuffer.empty()) {
	    if (txRetries <= 0) {
		trace() << "Transmission failed to " << txAddr;
		popTxBuffer();
	    } else {
		if (useRtsCts && txAddr != BROADCAST_ADDR) {
		    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_RTS,randomContentionInterval);
		} else {
		    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_DATA,randomContentionInterval);
		}
		return;
	    }
	}
	setMacState(MAC_STATE_ACTIVE);
    } else {
	//primaryWakeup == false
	setMacState(MAC_STATE_ACTIVE_SILENT);
    }
}
    
/* This function will create a new primary schedule for this node.
 * Most of the task is delegated to updateScheduleTable function
 * This function will only schedule a self message to resycnronise the newly created
 * schedule
 */
void TMacModule::createPrimarySchedule() {
    updateScheduleTable(frameTime,self,0);
    setTimer(SYNC_RENEW,resyncTime);
}

/* Helper function to change internal MAC state and print a debug statement if neccesary */
void TMacModule::setMacState(int newState) {
    if (macState == newState) return;
    if (printStateTransitions) {
	trace() << "state changed from " << stateDescr[macState] << " to " << stateDescr[newState];
    }
    macState = newState;
}


/* This function will update schedule table with the given values for wakeup time,
 * schedule ID and schedule SN
 */
void TMacModule::updateScheduleTable(simtime_t wakeup, int ID, int SN) {
    // First, search through existing schedules
    for (int i = 0; i < (int)scheduleTable.size(); i++) {
	//If schedule already exists
        if (scheduleTable[i].ID == ID) {
    	    //And SN is greater than ours, then update
    	    if (scheduleTable[i].SN < SN) {

    		//Calculate new frame offset for this schedule
    		simtime_t new_offset = getClock() - currentFrameStart + wakeup - frameTime;
    		trace() << "Resync successful for ID:"<<ID<<" old offset:"<<scheduleTable[i].offset<<" new offset:"<<new_offset;
    		scheduleTable[i].offset = new_offset;
    		scheduleTable[i].SN = SN;

    		if (i == 0) {
    		    //If the update came for primary schedule, then the next frame message has to be rescheduled
    		    setTimer(FRAME_START,wakeup);
    		    currentFrameStart += new_offset;
    		} else {
    		    //This is not primary schedule, check that offset value falls within the
    		    //interval: 0 < offset < frameTime
    		    if (scheduleTable[i].offset < 0) scheduleTable[i].offset += frameTime;
    		    if (scheduleTable[i].offset > frameTime) scheduleTable[i].offset -= frameTime;
    		}

	    } else if (scheduleTable[i].SN > SN) {
		/* TMAC received a sync with lower SN than what currently stored in the
		 * schedule table. With current TMAC implementation, this is not possible,
		 * however in future it may be neccesary to implement a unicast sync packet
		 * here to notify the source of this packet with the updated schedule
		 */
	    }

	    //found and updated the schedule, nothing else need to be done
	    return;
	}
    }

    //At this stage, the schedule was not found in the current table, so it has to be added
    TMacSchedule newSch;
    newSch.ID = ID;
    newSch.SN = SN;
    trace() << "Creating schedule ID:"<<ID<<", SN:"<<SN<<", wakeup:"<<wakeup;

    //Calculate the offset for the new schedule
    if (currentFrameStart == -1) {
        //If currentFrameStart is -1 then this schedule will be the new primary schedule
        //and it's offset will always be 0
	newSch.offset = 0;
    } else {
	//This schedule is not primary, it is necessary to calculate the offset from primary
	//schedule for this new schedule
	newSch.offset = getClock() - currentFrameStart + wakeup - frameTime;
    }

    //Add new schedule to the table
    scheduleTable.push_back(newSch);

    //If the new schedule is primary, more things need to be done:
    if (currentFrameStart == -1) {
	//This is new primary schedule, and since SYNC packet was received at this time, it is
	//safe to assume that nodes of this schedule are active and listening right now,
	//so active period can be safely extended
	currentFrameStart = activationTimeout = getClock();
	currentFrameStart += wakeup - frameTime;
	extendActivePeriod();

	//create and schedule the next frame message
	setTimer(FRAME_START,wakeup);

	//this flag indicates that this schedule has to be rebroadcasted
	needResync = 1;

	//MAC is reset to default state, allowing it to initiate and accept transmissions
	resetDefaultState();
    }
}

/* This function will handle a MAC frame received from the lower layer (physical or radio)
 * We try to see if the received packet is TMacPacket, otherwise we discard it
 * TMAC ignores values of RSSI and LQI
 */
void TMacModule::fromRadioLayer(cPacket *pkt, double RSSI, double LQI) {
    TMacPacket *macPkt = dynamic_cast<TMacPacket*>(pkt);
    if (macPkt == NULL) return;

    int source = macPkt->getSource();
    int destination = macPkt->getDestination();
    simtime_t nav = macPkt->getNav();

    if (destination != SELF_MAC_ADDRESS && destination != BROADCAST_MAC_ADDRESS) {
	if (macState == MAC_CARRIER_SENSE_FOR_TX_RTS && useFRTS) {
	    //FRTS would need to be implemented here, for now just keep silent
	}
	if (collisionResolution != 2 && nav > 0) setTimer(TRANSMISSION_TIMEOUT,nav);
	extendActivePeriod(nav);
	setMacState(MAC_STATE_ACTIVE_SILENT);
	return;
    }

    switch (macPkt->getType()) {

	/* received a RTS frame */
	case RTS_FRAME: {
	    //If this node is the destination, reply with a CTS, otherwise
	    //set a timeout and keep silent for the duration of communication
	    if (ctsPacket) cancelAndDelete(ctsPacket);
	    ctsPacket = new TMacPacket("TMAC CTS packet", MAC_LAYER_PACKET);
	    ctsPacket->setSource(SELF_MAC_ADDRESS);
	    ctsPacket->setDestination(source);
	    ctsPacket->setNav(nav - TX_TIME(ctsPacketSize));
	    ctsPacket->setType(CTS_FRAME);
	    ctsPacket->setByteLength(ctsPacketSize);
	    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_CTS);
	    break;
	}

	/* received a CTS frame */
	case CTS_FRAME: {
	    if(macState == MAC_STATE_WAIT_FOR_CTS) {
		if(TXBuffer.empty()) {
		    trace() << "WARNING: invalid MAC_STATE_WAIT_FOR_CTS while buffer is empty";
		    resetDefaultState();
		} else if(source == txAddr) {
		    cancelTimer(TRANSMISSION_TIMEOUT);
		    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_DATA);
		} else {
		    trace() << "WARNING: recieved unexpected CTS from " << source;
		    resetDefaultState();
		}
	    }
	    break;
	}

	/* received DATA frame */
	case DATA_FRAME: {
	    // Forward the frame to upper layer first
	    toNetworkLayer(macPkt->decapsulate());

	    // If the frame was sent to broadcast address, nothing else needs to be done
	    if (destination == BROADCAST_MAC_ADDRESS) break;

	    // If MAC was expecting this frame, clear the timeout
	    if (macState == MAC_STATE_WAIT_FOR_DATA) cancelTimer(TRANSMISSION_TIMEOUT);

	    // Create and send an ACK frame (since this node is the destination for DATA frame)
	    if (ackPacket) cancelAndDelete(ackPacket);
	    ackPacket = new TMacPacket("TMAC ACK packet", MAC_LAYER_PACKET);
	    ackPacket->setSource(SELF_MAC_ADDRESS);
	    ackPacket->setDestination(source);
	    ackPacket->setType(ACK_FRAME);
	    ackPacket->setByteLength(ackPacketSize);
	    ackPacket->setSequenceNumber(macPkt->getSequenceNumber());
	    performCarrierSense(MAC_CARRIER_SENSE_FOR_TX_ACK);
	    break;
	}

	/* received ACK frame */
	case ACK_FRAME: {
	    if (macState == MAC_STATE_WAIT_FOR_ACK && source == txAddr) {
		trace() << "Transmission succesful to " << txAddr;
		cancelTimer(TRANSMISSION_TIMEOUT);
		popTxBuffer();
		resetDefaultState();
	    }
	    break;
	}

	/* received SYNC frame */
	case SYNC_FRAME: {
	    // Schedule table is updated with values from the SYNC frame
	    updateScheduleTable(macPkt->getSync(), macPkt->getSyncId(), macPkt->getSequenceNumber());

	    // The state is reset to default allowing further transmissions in this frame
	    // (since SYNC frame does not intend to have an ACK or any other communications
	    // immediately after it)
	    resetDefaultState();
	    break;
	}
    }
}

/* This function handles carrier clear message, received from the radio module.
 * That is sent in a response to previous request to perform a carrier sense
 */
void TMacModule::carrierIsClear() {

    switch(macState) {

	/* MAC requested carrier sense to transmit an RTS packet */
	case MAC_CARRIER_SENSE_FOR_TX_RTS: {
	    if (TXBuffer.empty()) {
		trace() << "WARNING! BUFFER_IS_EMPTY in MAC_CARRIER_SENSE_FOR_TX_RTS, will reset state";
		resetDefaultState();
		break;
	    }

	    // create and send RTS frame
	    if (rtsPacket) cancelAndDelete(rtsPacket);
	    rtsPacket = new TMacPacket("RTS message", MAC_LAYER_PACKET);
	    rtsPacket->setSource(SELF_MAC_ADDRESS);
	    rtsPacket->setDestination(txAddr);
	    rtsPacket->setNav(TX_TIME(rtsPacketSize) + TX_TIME(ackPacketSize) + TX_TIME(TXBuffer.front()->getByteLength()) + NAV_EXTENSION);
 	    rtsPacket->setType(RTS_FRAME);
	    rtsPacket->setByteLength(rtsPacketSize);
	    toRadioLayer(rtsPacket);
	    if (useRtsCts) txRetries--;
	    packetsSent["RTS"]++;
	    collectOutput("Sent packets breakdown","RTS");
	    rtsPacket = NULL;

	    // update MAC state
	    setMacState(MAC_STATE_WAIT_FOR_CTS);

	    // create a timeout for expecting a CTS reply
	    setTimer(TRANSMISSION_TIMEOUT, TX_TIME(rtsPacketSize) + waitTimeout);
	    break;
	}

	/* MAC requested carrier sense to transmit a SYNC packet */
	case MAC_CARRIER_SENSE_FOR_TX_SYNC: {
	    // SYNC packet was created in scheduleSyncPacket function
	    if (syncPacket != NULL) {
		// Send SYNC packet to radio
		toRadioLayer(syncPacket);

		packetsSent["SYNC"]++;
		collectOutput("Sent packets breakdown","SYNC");
		syncPacket = NULL;

		// Clear the resync flag
		needResync = 0;

		// update MAC state
		setMacState(MAC_STATE_IN_TX);

		// create a timeout for this transmission - nothing is expected in reply
		// so MAC is only waiting for the RADIO to finish the packet transmission
		setTimer(TRANSMISSION_TIMEOUT,TX_TIME(syncPacketSize));

	    } else {
		trace() << "WARNING: Invalid MAC_CARRIER_SENSE_FOR_TX_SYNC while syncPacket undefined";
		resetDefaultState();
	    }
	    break;
	}

	/* MAC requested carrier sense to transmit a CTS packet */
	case MAC_CARRIER_SENSE_FOR_TX_CTS: {
	    // CTS packet was created when RTS was received
	    if (ctsPacket != NULL) {
		// Send CTS packet to radio
		toRadioLayer(ctsPacket);
		packetsSent["CTS"]++;
		collectOutput("Sent packets breakdown","CTS");
		ctsPacket = NULL;

		// update MAC state
		setMacState(MAC_STATE_WAIT_FOR_DATA);

		// create a timeout for expecting a DATA packet reply
		setTimer(TRANSMISSION_TIMEOUT,TX_TIME(ctsPacketSize) + waitTimeout);

	    } else {
		trace() << "WARNING: Invalid MAC_CARRIER_SENSE_FOR_TX_CTS while ctsPacket undefined";
		resetDefaultState();
	    }
	    break;
	}

	/* MAC requested carrier sense to transmit DATA packet */
	case MAC_CARRIER_SENSE_FOR_TX_DATA: {
	    if (TXBuffer.empty()) {
		trace() << "WARNING: Invalid MAC_CARRIER_SENSE_FOR_TX_DATA while TX buffer is empty";
		resetDefaultState();
		break;
	    }


	    // create a copy of first message in the TX buffer and send it to the radio
	    toRadioLayer(TXBuffer.front()->dup());
	    packetsSent["DATA"]++;
	    collectOutput("Sent packets breakdown","DATA");

	    //update MAC state based on transmission time and destination address
	    double txTime = TX_TIME(TXBuffer.front()->getByteLength());

	    if (txAddr == BROADCAST_MAC_ADDRESS) {
		// This packet is broadcast, so no reply will be received
		// The packet can be cleared from transmission buffer
		// and MAC timeout is only to allow RADIO to finish the transmission
		popTxBuffer();
		setMacState(MAC_STATE_IN_TX);
		setTimer(TRANSMISSION_TIMEOUT,txTime);
	    } else {
		// This packet is unicast, so MAC will be expecting an ACK
		// packet in reply, so the timeout is longer
		// If we are not using RTS/CTS exchange, then this attempt
		// also decreases the txRetries counter 
		// (NOTE: with RTS/CTS exchange sending RTS packet decrements counter)
		if (!useRtsCts) txRetries--;
	        setMacState(MAC_STATE_WAIT_FOR_ACK);
	        setTimer(TRANSMISSION_TIMEOUT,txTime + waitTimeout);
	    }

	    extendActivePeriod(txTime);

	    //update RADIO state
	    toRadioLayer(createRadioCommand(SET_STATE,TX));
	    break;
	}

	/* MAC requested carrier sense to transmit ACK packet */
	case MAC_CARRIER_SENSE_FOR_TX_ACK: {
	    // ACK packet was created when MAC received a DATA packet.
	    if(ackPacket != NULL) {
		// Send ACK packet to the radio
		toRadioLayer(ackPacket);
		packetsSent["ACK"]++;
		collectOutput("Sent packets breakdown","ACK");
		ackPacket = NULL;

		// update MAC state
		setMacState(MAC_STATE_IN_TX);

		// create a timeout for this transmission - nothing is expected in reply
		// so MAC is only waiting for the RADIO to finish the packet transmission
		setTimer(TRANSMISSION_TIMEOUT,TX_TIME(ackPacketSize));
		extendActivePeriod(TX_TIME(ackPacketSize));
	    } else {
		trace() << "WARNING: Invalid MAC_STATE_WAIT_FOR_ACK while ackPacket undefined";
		resetDefaultState();
	    }
	    break;
	}

	/* MAC requested carrier sense before going to sleep */
	case MAC_CARRIER_SENSE_BEFORE_SLEEP: {
	    // primaryWakeup flag is cleared (in case the node will wake up in the current frame
	    // for a secondary schedule)
	    primaryWakeup = false;

	    // update MAC and RADIO states
	    toRadioLayer(createRadioCommand(SET_STATE,SLEEP));
	    setMacState(MAC_STATE_SLEEP);
	    break;
	}
    }
}

/* This function will create a request to RADIO module to perform carrier sense.
 * MAC state is important when performing CS, so setMacState is always called here.
 * delay allows to perform a carrier sense after a choosen delay (useful for
 * randomisation of transmissions)
 */
void TMacModule::performCarrierSense(int newState, simtime_t delay) {
    setMacState(newState);
    setTimer(CARRIER_SENSE,delay);
}

/* This function will extend active period for MAC, ensuring that the remaining active
 * time it is not less than listenTimeout value. Also a check TA message is scheduled here
 * to allow the node to go to sleep if activation timeout expires
 */
void TMacModule::extendActivePeriod(simtime_t extra) {
    simtime_t curTime = getClock();
    if (conservativeTA) {
	curTime += extra;
	while (activationTimeout < curTime) { activationTimeout += listenTimeout; }
	if (curTime + listenTimeout < activationTimeout) return;
	activationTimeout += listenTimeout;
    } else if (activationTimeout < curTime + listenTimeout + extra) {
	activationTimeout = curTime + listenTimeout + extra;
    }
    setTimer(CHECK_TA,activationTimeout-curTime);
}

/* This function will check the transmission buffer, and if it is not empty, it will update
 * current communication parameters: txAddr and txRetries
 */
void TMacModule::checkTxBuffer() {
    if (TXBuffer.empty()) return;
    TMacPacket *macPkt = check_and_cast<TMacPacket*>(TXBuffer.front());
    txAddr = macPkt->getDestination();
    txRetries = maxTxRetries;
    txSequenceNum = 0;
}

/* This function will remove the first packet from MAC transmission buffer
 * checkTxBuffer is called in case there are still packets left in the buffer to transmit
 */
void TMacModule::popTxBuffer() {
    cancelAndDelete(TXBuffer.front());
    TXBuffer.pop();
    checkTxBuffer();
}
