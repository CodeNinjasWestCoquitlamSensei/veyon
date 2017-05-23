/*
 * VncServerProtocol.cpp - implementation of the VncServerProtocol class
 *
 * Copyright (c) 2017 Tobias Doerffel <tobydox/at/users/dot/sf/dot/net>
 *
 * This file is part of Veyon - http://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "VeyonCore.h"

#ifdef VEYON_BUILD_WIN32
#include <winsock2.h>
#endif

#include <QHostAddress>
#include <QTcpSocket>

#include "rfb/rfbproto.h"

#include "AuthenticationCredentials.h"
#include "ServerAuthenticationManager.h"
#include "ServerAccessControlManager.h"
#include "VariantArrayMessage.h"
#include "VncServerClient.h"
#include "VncServerProtocol.h"


VncServerProtocol::VncServerProtocol( QTcpSocket* socket,
									  VncServerClient* client,
									  ServerAuthenticationManager& serverAuthenticationManager,
									  ServerAccessControlManager& serverAccessControlManager ) :
	m_socket( socket ),
	m_client( client ),
	m_serverAuthenticationManager( serverAuthenticationManager ),
	m_serverAccessControlManager( serverAccessControlManager ),
	m_serverInitMessage()
{
	m_client->setAccessControlState( VncServerClient::AccessControlInit );

	connect( &m_serverAccessControlManager, &ServerAccessControlManager::accessControlFinished,
			 this, &VncServerProtocol::finishAccessControl );

}



VncServerProtocol::~VncServerProtocol()
{
}



VncServerProtocol::State VncServerProtocol::state() const
{
	return m_client->protocolState();
}



void VncServerProtocol::start()
{
	if( state() == Disconnected )
	{
		char protocol[sz_rfbProtocolVersionMsg+1];

		sprintf( protocol, rfbProtocolVersionFormat, 3, 8 );

		m_socket->write( protocol, sz_rfbProtocolVersionMsg );

		setState( Protocol );
	}
}



bool VncServerProtocol::read()
{
	switch( state() )
	{
	case Protocol:
		return readProtocol();

	case SecurityInit:
		return receiveSecurityTypeResponse();

	case AuthenticationTypes:
		return receiveAuthenticationTypeResponse();

	case Authenticating:
		return receiveAuthenticationMessage();

	case AccessControl:
		return processAccessControl();

	case FramebufferInit:
		return processFramebufferInit();

	case Close:
		qDebug( "VncServerProtocol::read(): closing connection per protocol state" );
		m_socket->close();
		break;

	default:
		break;
	}

	return false;
}



void VncServerProtocol::setState( VncServerProtocol::State state )
{
	m_client->setProtocolState( state );
}



bool VncServerProtocol::readProtocol()
{
	if( m_socket->bytesAvailable() == sz_rfbProtocolVersionMsg )
	{
		char protocol[sz_rfbProtocolVersionMsg+1];
		m_socket->read( protocol, sz_rfbProtocolVersionMsg );
		protocol[sz_rfbProtocolVersionMsg] = 0;

		int protocolMajor = 0, protocolMinor = 0;

		if( sscanf( protocol, rfbProtocolVersionFormat, &protocolMajor, &protocolMinor ) != 2 )
		{
			qCritical( "VncServerProtocol:::readProtocol(): protocol initialization failed" );
			m_socket->close();
			return false;
		}

		setState( SecurityInit );

		return sendSecurityTypes();
	}

	return false;
}



bool VncServerProtocol::sendSecurityTypes()
{
	// send list of supported security types
	const char securityTypeList[2] = { 1, rfbSecTypeVeyon };
	m_socket->write( securityTypeList, sizeof( securityTypeList ) );

	return true;
}



bool VncServerProtocol::receiveSecurityTypeResponse()
{
	if( m_socket->bytesAvailable() >= 1 )
	{
		char chosenSecurityType = 0;

		if( m_socket->read( &chosenSecurityType, sizeof(chosenSecurityType) ) != sizeof(chosenSecurityType) ||
				chosenSecurityType != rfbSecTypeVeyon )
		{
			qCritical( "VncServerProtocol:::receiveSecurityTypeResponse(): protocol initialization failed" );
			m_socket->close();

			return false;
		}

		setState( AuthenticationTypes );

		return sendAuthenticationTypes();
	}

	return false;
}



bool VncServerProtocol::sendAuthenticationTypes()
{
	VariantArrayMessage message( m_socket );
	message.write( m_serverAuthenticationManager.supportedAuthTypes().count() );

	const auto supportedAuthTypes = m_serverAuthenticationManager.supportedAuthTypes();

	for( auto authType : supportedAuthTypes )
	{
		message.write( authType );
	}

	return message.send();
}



bool VncServerProtocol::receiveAuthenticationTypeResponse()
{
	VariantArrayMessage message( m_socket );

	if( message.isReadyForReceive() && message.receive() )
	{
		auto chosenAuthType = message.read().value<RfbVeyonAuth::Type>();

		if( m_serverAuthenticationManager.supportedAuthTypes().contains( chosenAuthType ) == false )
		{
			qCritical( "VncServerProtocol:::receiveAuthenticationTypeResponse(): unsupported authentication type chosen by client!" );
			m_socket->close();

			return false;
		}

		if( chosenAuthType == RfbVeyonAuth::None )
		{
			qWarning( "VncServerProtocol::receiveAuthenticationTypeResponse(): skipping authentication." );
			setState( AccessControl );
			return true;
		}

		const QString username = message.read().toString();

		m_client->setAuthType( chosenAuthType );
		m_client->setUsername( username );
		m_client->setHostAddress( m_socket->peerAddress().toString() );

		setState( Authenticating );

		// send auth ack message
		VariantArrayMessage( m_socket ).send();

		// init authentication
		VariantArrayMessage dummyMessage( m_socket );
		processAuthentication( dummyMessage );
	}

	return false;
}



bool VncServerProtocol::receiveAuthenticationMessage()
{
	VariantArrayMessage message( m_socket );

	if( message.isReadyForReceive() && message.receive() )
	{
		return processAuthentication( message );
	}

	return false;
}



bool VncServerProtocol::processAuthentication( VariantArrayMessage& message )
{
	m_serverAuthenticationManager.processAuthenticationMessage( m_client, message );

	switch( m_client->authState() )
	{
	case VncServerClient::AuthFinishedSuccess:
	{
		uint32_t authResult = qToBigEndian<uint32_t>(rfbVncAuthOK);
		m_socket->write( (char *) &authResult, sizeof(authResult) );

		setState( AccessControl );
		return true;
	}

	case VncServerClient::AuthFinishedFail:
		qCritical( "VncServerProtocol:::receiveAuthenticationMessage(): authentication failed - closing connection" );
		m_socket->close();

		return false;

	default:
		break;
	}

	return false;
}



bool VncServerProtocol::processAccessControl()
{
	// perform access control via ServerAccessControl manager if either
	// client just entered access control or is still waiting to be
	// processed (e.g. desktop access dialog already active for a different connection)
	if( m_client->accessControlState() == VncServerClient::AccessControlInit ||
			m_client->accessControlState() == VncServerClient::AccessControlWaiting )
	{
		m_serverAccessControlManager.addClient( m_client );
	}

	switch( m_client->accessControlState() )
	{
	case VncServerClient::AccessControlSuccessful:
		setState( FramebufferInit );
		return true;

	case VncServerClient::AccessControlPending:
	case VncServerClient::AccessControlWaiting:
		break;

	default:
		qCritical( "VncServerProtocol:::processAccessControl(): access control failed - closing connection" );
		m_socket->close();
		break;
	}

	return false;
}



bool VncServerProtocol::processFramebufferInit()
{
	if( m_socket->bytesAvailable() >= sz_rfbClientInitMsg &&
			m_serverInitMessage.isEmpty() == false )
	{
		// just read client init message without further evaluation
		m_socket->read( sz_rfbClientInitMsg );

		m_socket->write( m_serverInitMessage );

		setState( Running );

		return true;
	}

	return false;
}



void VncServerProtocol::finishAccessControl( VncServerClient* client )
{
	if( client == m_client && processAccessControl() )
	{
		while( read() )
		{
		}
	}
}
