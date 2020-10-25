//
// usbhostcontroller.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2020  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbhcirootport.h>
#include <circle/usb/usbstandardhub.h>
#include <circle/usb/usbdevice.h>
#include <circle/timer.h>
#include <assert.h>

enum TPortStatusEventType
{
	PortStatusEventFromRootPort,
	PortStatusEventFromHub,
	PortStatusEventDeviceRemoved
};

struct TPortStatusEvent
{
	TPortStatusEventType Type;

	union
	{
		CUSBHCIRootPort	*pRootPort;
		CUSBStandardHub *pHub;
		CUSBDevice	*pDevice;
	};

	unsigned nCreatedTicks;		// for PortStatusEventDeviceRemoved only
};

boolean CUSBHostController::s_bPlugAndPlay;

CUSBHostController *CUSBHostController::s_pThis = 0;

CUSBHostController::CUSBHostController (boolean bPlugAndPlay)
:	m_bFirstUpdateCall (TRUE)
{
	s_pThis = this;

	s_bPlugAndPlay = bPlugAndPlay;
}

CUSBHostController::~CUSBHostController (void)
{
	s_pThis = 0;
}
	
int CUSBHostController::GetDescriptor (CUSBEndpoint	*pEndpoint, 
				       unsigned char	 ucType,
				       unsigned char	 ucIndex,
				       void		*pBuffer,
				       unsigned		 nBufSize,
				       unsigned char	 ucRequestType,
				       unsigned short	 wIndex)
{
	return ControlMessage (pEndpoint,
			       ucRequestType, GET_DESCRIPTOR,
			       (ucType << 8) | ucIndex, wIndex,
			       pBuffer, nBufSize);
}

boolean CUSBHostController::SetAddress (CUSBEndpoint *pEndpoint, u8 ucDeviceAddress)
{
	if (ControlMessage (pEndpoint, REQUEST_OUT, SET_ADDRESS, ucDeviceAddress, 0, 0, 0) < 0)
	{
		return FALSE;
	}
	
	CTimer::Get ()->MsDelay (50);		// see USB 2.0 spec (tDSETADDR)
	
	return TRUE;
}

boolean CUSBHostController::SetConfiguration (CUSBEndpoint *pEndpoint, u8 ucConfigurationValue)
{
	if (ControlMessage (pEndpoint, REQUEST_OUT, SET_CONFIGURATION, ucConfigurationValue, 0, 0, 0) < 0)
	{
		return FALSE;
	}
	
	CTimer::Get ()->MsDelay (50);
	
	return TRUE;
}

int CUSBHostController::ControlMessage (CUSBEndpoint *pEndpoint,
					u8 ucRequestType, u8 ucRequest,
					u16 usValue, u16 usIndex,
					void *pData, u16 usDataSize)
{
	TSetupData *pSetup = new TSetupData;
	assert (pSetup != 0);

	pSetup->bmRequestType = ucRequestType;
	pSetup->bRequest      = ucRequest;
	pSetup->wValue	      = usValue;
	pSetup->wIndex	      = usIndex;
	pSetup->wLength	      = usDataSize;

	CUSBRequest URB (pEndpoint, pData, usDataSize, pSetup);

	int nResult = -1;

	if (SubmitBlockingRequest (&URB))
	{
		nResult = URB.GetResultLength ();
	}
	
	delete pSetup;

	return nResult;
}

int CUSBHostController::Transfer (CUSBEndpoint *pEndpoint, void *pBuffer, unsigned nBufSize,
				  unsigned nTimeoutMs)
{
	CUSBRequest URB (pEndpoint, pBuffer, nBufSize);

	if (!SubmitBlockingRequest (&URB, nTimeoutMs))
	{
		return -1;
	}

	return URB.GetResultLength ();
}

boolean CUSBHostController::IsPlugAndPlay (void)
{
	return s_bPlugAndPlay;
}

boolean CUSBHostController::UpdatePlugAndPlay (void)
{
	assert (s_bPlugAndPlay);

	boolean bResult = m_bFirstUpdateCall;
	m_bFirstUpdateCall = FALSE;

	m_SpinLock.Acquire ();

	TPtrListElement *pElement = m_EventList.GetFirst ();
	while (pElement != 0)
	{
		TPortStatusEvent *pEvent = (TPortStatusEvent *) m_EventList.GetPtr (pElement);

		boolean bEventHandled = FALSE;

		m_SpinLock.Release ();

		assert (pEvent != 0);
		switch (pEvent->Type)
		{
		case PortStatusEventFromRootPort:
			assert (pEvent->pRootPort != 0);
			pEvent->pRootPort->HandlePortStatusChange ();
			bEventHandled = TRUE;
			break;

		case PortStatusEventFromHub:
			assert (pEvent->pHub != 0);
			pEvent->pHub->HandlePortStatusChange ();
			bEventHandled = TRUE;
			break;

		case PortStatusEventDeviceRemoved:
			assert (pEvent->pDevice != 0);
			if (   pEvent->pDevice->ShutdownDevice ()
			    || CTimer::Get ()->GetTicks () - pEvent->nCreatedTicks >= MSEC2HZ (150))
			{
				delete pEvent->pDevice;
				bEventHandled = TRUE;
			}
			break;

		default:
			assert (0);
			break;
		}

		m_SpinLock.Acquire ();

		TPtrListElement *pNextElement = m_EventList.GetNext (pElement);

		if (bEventHandled)
		{
			m_EventList.Remove (pElement);

			delete pEvent;

			bResult = TRUE;
		}

		pElement = pNextElement;
	}

	m_SpinLock.Release ();

	return bResult;
}

void CUSBHostController::PortStatusChanged (CUSBHCIRootPort *pRootPort)
{
	assert (s_bPlugAndPlay);
	assert (pRootPort != 0);

	TPortStatusEvent *pEvent = new TPortStatusEvent;
	assert (pEvent != 0);
	pEvent->Type = PortStatusEventFromRootPort;
	pEvent->pRootPort = pRootPort;

	m_SpinLock.Acquire ();

	TPtrListElement *pPrevElement = 0;
	TPtrListElement *pElement = m_EventList.GetFirst ();
	while (pElement != 0)					// find last element
	{
		pPrevElement = pElement;
		pElement = m_EventList.GetNext (pElement);
	}

	m_EventList.InsertAfter (pPrevElement, pEvent);		// append to list

	m_SpinLock.Release ();
}

void CUSBHostController::PortStatusChanged (CUSBStandardHub *pHub)
{
	assert (s_bPlugAndPlay);
	assert (pHub != 0);

	TPortStatusEvent *pEvent = new TPortStatusEvent;
	assert (pEvent != 0);
	pEvent->Type = PortStatusEventFromHub;
	pEvent->pHub = pHub;

	m_SpinLock.Acquire ();

	TPtrListElement *pPrevElement = 0;
	TPtrListElement *pElement = m_EventList.GetFirst ();
	while (pElement != 0)					// find last element
	{
		pPrevElement = pElement;
		pElement = m_EventList.GetNext (pElement);
	}

	m_EventList.InsertAfter (pPrevElement, pEvent);		// append to list

	m_SpinLock.Release ();
}

CUSBHostController *CUSBHostController::Get (void)
{
	assert (s_pThis != 0);
	return s_pThis;
}

void CUSBHostController::RemoveDevice (CUSBDevice *pDevice)
{
	assert (pDevice != 0);

	// immediately remove device, if possible
	if (   !IsPlugAndPlay ()
	    || pDevice->ShutdownDevice ())
	{
		delete pDevice;

		return;
	}

	// otherwise queue event
	TPortStatusEvent *pEvent = new TPortStatusEvent;
	assert (pEvent != 0);
	pEvent->Type = PortStatusEventDeviceRemoved;
	pEvent->pDevice = pDevice;
	pEvent->nCreatedTicks = CTimer::Get ()->GetTicks ();

	m_SpinLock.Acquire ();

	TPtrListElement *pPrevElement = 0;
	TPtrListElement *pElement = m_EventList.GetFirst ();
	while (pElement != 0)					// find last element
	{
		pPrevElement = pElement;
		pElement = m_EventList.GetNext (pElement);
	}

	m_EventList.InsertAfter (pPrevElement, pEvent);		// append to list

	m_SpinLock.Release ();
}
