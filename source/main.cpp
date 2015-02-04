﻿// Required interfaces
#define IVENGINESERVER_INTERFACE
#define IVENGINECLIENT_INTERFACE
#define ICVAR_INTERFACE

#include <main.hpp>
#include <GarrysMod/Lua/LuaInterface.h>
#include <net.h>
#include <protocol.h>
#include <gl_hooks.hpp>
#include <gl_bitbuf_write.hpp>
#include <gl_bitbuf_read.hpp>
#include <gl_cnetchan.hpp>
#include <gl_inetchannelhandler.hpp>
#include <gl_subchannel_t.hpp>
#include <gl_datafragments_t.hpp>
#include <gl_filehandle_t.hpp>
#include <gl_ucharptr.hpp>
#include <gl_netadr_t.hpp>
#include <gl_inetworkstringtablecontainer.hpp>
#include <gl_inetworkstringtable.hpp>
#include <gl_igameeventmanager2.hpp>
#include <gl_igameevent.hpp>
#include <symbolfinder.hpp>

// Platform definitions

#if defined _WIN32

#undef INVALID_HANDLE_VALUE

#include <windows.h>

#define BEGIN_MEMEDIT( addr, size ) \
{ \
	DWORD previous; \
	VirtualProtect( addr, \
			size, \
			PAGE_EXECUTE_READWRITE, \
			&previous )

#define FINISH_MEMEDIT( addr, size ) \
	VirtualProtect( addr, \
			size, \
			previous, \
			nullptr ); \
}

static const char *engine_lib = "engine.dll";
static const char *client_lib = "client.dll";
static const char *server_lib = "server.dll";

static const char *CNetChan_ProcessMessages_sig = "\x55\x8B\xEC\x83\xEC\x2C\x53\x89\x4D\xFC";
static size_t CNetChan_ProcessMessages_siglen = 10;

static size_t netpatch_len = 6;
static const char *netpatch_old = "\x0F\x84\xC0\x00\x00\x00";
static const char *netpatch_new = "\xE9\xC1\x00\x00\x00\x90";

static size_t netchunk_sig_offset = 15;
static const char *netchunk_sig = "\x83\x78\x30\x00\x53\x8B\x5D\x08\x56\x8B\xF1\x57\x89\x75\xFC\x74";
static size_t netchunk_siglen = 16;

#elif defined __linux

#include <sys/mman.h>
#include <unistd.h>

inline uint8_t *PageAlign( uint8_t *addr, long page )
{
	return addr - ( (DWORD)addr % page );
}

#define BEGIN_MEMEDIT( addr, size ) \ 
{ \
	long page = sysconf( _SC_PAGESIZE ); \
	mprotect( PageAlign( static_cast<uint8_t *>( addr ), page ), \
			page, PROT_EXEC | PROT_READ | PROT_WRITE )

#define FINISH_MEMEDIT( addr, size ) \
	mprotect( PageAlign( static_cast<uint8_t *>( addr ), page ), \
			page, PROT_EXEC | PROT_READ ); \
}

static const char *engine_lib = "engine.so";
static const char *client_lib = "client.so";
static const char *server_lib = "server.so";

static const char *CNetChan_ProcessMessages_sig = "@_ZN8CNetChan15ProcessMessagesER7bf_read";
static size_t CNetChan_ProcessMessages_siglen = 0;

static size_t netpatch_len = 6;
static const char *netpatch_old = "\x0F\x85\xC9\x00\x00\x00";
static const char *netpatch_new = "\xE9\x01\x00\x00\x00\90";

static size_t netchunk_sig_offset = 8;
static const char *netchunk_sig = "\x85\xFF\x8D\x04\x91\x89\x46\x10";
static size_t netchunk_siglen = 8;

#elif defined __APPLE__

#include <sys/mman.h>
#include <unistd.h>

inline uint8_t *PageAlign( uint8_t *addr, long page )
{
	return addr - ( (DWORD)addr % page );
}

#define BEGIN_MEMEDIT( addr, size ) \ 
{ \
	long page = sysconf( _SC_PAGESIZE ); \
	mprotect( PageAlign( static_cast<uint8_t *>( addr ), page ), \
			page, PROT_EXEC | PROT_READ | PROT_WRITE )

#define FINISH_MEMEDIT( addr, size ) \
	mprotect( PageAlign( static_cast<uint8_t *>( addr ), page ), \
			page, PROT_EXEC | PROT_READ ); \
}

static const char *engine_lib = "engine.dylib";
static const char *client_lib = "client.dylib";
static const char *server_lib = "server.dylib";

static const char *CNetChan_ProcessMessages_sig = "@__ZN8CNetChan15ProcessMessagesER7bf_read";
static size_t CNetChan_ProcessMessages_siglen = 0;

static size_t netpatch_len = 0;
static const char *netpatch_old = "";
static const char *netpatch_new = "";

static size_t netchunk_sig_offset = 0;
static const char *netchunk_sig = "";
static size_t netchunk_siglen = 0;

#endif

lua_State *global_state = nullptr;

// Enable/disable SendDatagram hooking
bool g_bPatchedNetChunk = false;

// Interfaces
static CDllDemandLoader engine_loader( engine_lib );
CreateInterfaceFn fnEngineFactory = nullptr;

IVEngineServer *g_pEngineServer = nullptr;
IVEngineClient *g_pEngineClient = nullptr;

ICvar *g_pCVarClient = nullptr;
ICvar *g_pCVarServer = nullptr;

// CNetChan::ProcessMessages function pointer
CNetChan_ProcessMessages_T CNetChan_ProcessMessages_O = nullptr;

void TypeError( lua_State *state, const char *name, int index )
{
	static_cast<GarrysMod::Lua::ILuaInterface *>( LUA )->TypeError( name, index );
}

// Checks if the server is dedicated
GLBL_FUNCTION( sourcenet_isDedicatedServer )
{
	LUA->PushBool( g_pEngineServer->IsDedicatedServer( ) );
	return 1;
}

// Module load
GMOD_MODULE_OPEN( )
{
	global_state = state;

	SymbolFinder symfinder;
	
	fnEngineFactory = engine_loader.GetFactory( );
	if( fnEngineFactory == nullptr )
		LUA->ThrowError( "failed to retrieve engine factory function" );

	g_pEngineServer = static_cast<IVEngineServer *>( fnEngineFactory( "VEngineServer021", nullptr ) );
	if( g_pEngineServer == nullptr )
		LUA->ThrowError( "failed to retrieve server engine interface" );

	if( !g_pEngineServer->IsDedicatedServer( ) )
	{
		g_pEngineClient = static_cast<IVEngineClient *>( fnEngineFactory( "VEngineClient015", nullptr ) );
		if( g_pEngineClient == nullptr )
			LUA->ThrowError( "failed to retrieve client engine interface" );

		ICvar **cvar = static_cast<ICvar **>( symfinder.ResolveOnBinary( client_lib, "@cvar" ) );
		if( cvar == nullptr )
			LUA->ThrowError( "failed to retrieve client cvar interface (1)" );

		g_pCVarClient = *cvar;
		if( g_pCVarClient == nullptr )
			LUA->ThrowError( "failed to retrieve client cvar interface (2)" );
	}

#if defined SOURCENET_SERVER
	
	if( g_pEngineServer->IsDedicatedServer( ) )
	{
		ICvar **cvar = static_cast<ICvar **>( symfinder.ResolveOnBinary( server_lib, "@cvar" ) );
		if( cvar == nullptr )
			LUA->ThrowError( "failed to retrieve server cvar interface (1)" );

		g_pCVarServer = *cvar;
		if( g_pCVarServer == nullptr )
			LUA->ThrowError( "failed to retrieve server cvar interface (2)" );
	}

#endif

	LUA->PushSpecial( GarrysMod::Lua::SPECIAL_GLOB );

	BEGIN_ENUM_REGISTRATION( UpdateType );
		REG_ENUM( UpdateType, EnterPVS );
		REG_ENUM( UpdateType, LeavePVS );
		REG_ENUM( UpdateType, DeltaEnt );
		REG_ENUM( UpdateType, PreserveEnt );
		REG_ENUM( UpdateType, Finished );
		REG_ENUM( UpdateType, Failed );
	END_ENUM_REGISTRATION( );

	REG_GLBL_NUMBER( FHDR_ZERO );
	REG_GLBL_NUMBER( FHDR_LEAVEPVS );
	REG_GLBL_NUMBER( FHDR_DELETE );
	REG_GLBL_NUMBER( FHDR_ENTERPVS );

	REG_GLBL_STRING( INSTANCE_BASELINE_TABLENAME );
	REG_GLBL_STRING( LIGHT_STYLES_TABLENAME );
	REG_GLBL_STRING( USER_INFO_TABLENAME );
	REG_GLBL_STRING( SERVER_STARTUP_DATA_TABLENAME );

	REG_GLBL_NUMBER( NET_MESSAGE_BITS );

	REG_GLBL_NUMBER( net_NOP );
	REG_GLBL_NUMBER( net_Disconnect );
	REG_GLBL_NUMBER( net_File );
	REG_GLBL_NUMBER( net_LastControlMessage );

	REG_GLBL_NUMBER( net_Tick );
	REG_GLBL_NUMBER( net_StringCmd );
	REG_GLBL_NUMBER( net_SetConVar );
	REG_GLBL_NUMBER( net_SignonState );

	REG_GLBL_NUMBER( svc_ServerInfo );
	REG_GLBL_NUMBER( svc_SendTable );
	REG_GLBL_NUMBER( svc_ClassInfo );
	REG_GLBL_NUMBER( svc_SetPause );
	REG_GLBL_NUMBER( svc_CreateStringTable );
	REG_GLBL_NUMBER( svc_UpdateStringTable );
	REG_GLBL_NUMBER( svc_VoiceInit );
	REG_GLBL_NUMBER( svc_VoiceData );
	REG_GLBL_NUMBER( svc_Print );
	REG_GLBL_NUMBER( svc_Sounds );
	REG_GLBL_NUMBER( svc_SetView );
	REG_GLBL_NUMBER( svc_FixAngle );
	REG_GLBL_NUMBER( svc_CrosshairAngle );
	REG_GLBL_NUMBER( svc_BSPDecal );
	REG_GLBL_NUMBER( svc_UserMessage );
	REG_GLBL_NUMBER( svc_EntityMessage );
	REG_GLBL_NUMBER( svc_GameEvent );
	REG_GLBL_NUMBER( svc_PacketEntities );
	REG_GLBL_NUMBER( svc_TempEntities );
	REG_GLBL_NUMBER( svc_Prefetch );
	REG_GLBL_NUMBER( svc_Menu );
	REG_GLBL_NUMBER( svc_GameEventList );
	REG_GLBL_NUMBER( svc_GetCvarValue );
	REG_GLBL_NUMBER( svc_CmdKeyValues );
	REG_GLBL_NUMBER( svc_GMod_ServerToClient );
	REG_GLBL_NUMBER( SVC_LASTMSG );

	REG_GLBL_NUMBER( clc_ClientInfo );
	REG_GLBL_NUMBER( clc_Move );
	REG_GLBL_NUMBER( clc_VoiceData );
	REG_GLBL_NUMBER( clc_BaselineAck );
	REG_GLBL_NUMBER( clc_ListenEvents );
	REG_GLBL_NUMBER( clc_RespondCvarValue );
	REG_GLBL_NUMBER( clc_FileCRCCheck );
	REG_GLBL_NUMBER( clc_CmdKeyValues );
	REG_GLBL_NUMBER( clc_FileMD5Check );
	REG_GLBL_NUMBER( clc_GMod_ClientToServer );
	REG_GLBL_NUMBER( CLC_LASTMSG );

	REG_GLBL_NUMBER( RES_FATALIFMISSING );
	REG_GLBL_NUMBER( RES_PRELOAD );

	REG_GLBL_NUMBER( SIGNONSTATE_NONE );
	REG_GLBL_NUMBER( SIGNONSTATE_CHALLENGE );
	REG_GLBL_NUMBER( SIGNONSTATE_CONNECTED );
	REG_GLBL_NUMBER( SIGNONSTATE_NEW );
	REG_GLBL_NUMBER( SIGNONSTATE_PRESPAWN );
	REG_GLBL_NUMBER( SIGNONSTATE_SPAWN );
	REG_GLBL_NUMBER( SIGNONSTATE_FULL );
	REG_GLBL_NUMBER( SIGNONSTATE_CHANGELEVEL );

	REG_GLBL_NUMBER( MAX_STREAMS );
	REG_GLBL_NUMBER( FRAG_NORMAL_STREAM );
	REG_GLBL_NUMBER( FRAG_FILE_STREAM );

	REG_GLBL_NUMBER( MAX_RATE );
	REG_GLBL_NUMBER( MIN_RATE );
	REG_GLBL_NUMBER( DEFAULT_RATE );

	REG_GLBL_NUMBER( MAX_FRAGMENT_SIZE );
	REG_GLBL_NUMBER( MAX_SUBCHANNELS );
	REG_GLBL_NUMBER( MAX_FILE_SIZE );

	REG_GLBL_NUMBER( FLOW_OUTGOING );
	REG_GLBL_NUMBER( FLOW_INCOMING );
	REG_GLBL_NUMBER( MAX_FLOWS );

	REG_GLBL_NUMBER( MAX_CUSTOM_FILES );

	BEGIN_META_REGISTRATION( sn4_bf_write );
		REG_META_FUNCTION( sn4_bf_write, GetBasePointer );
		REG_META_FUNCTION( sn4_bf_write, GetMaxNumBits );
		REG_META_FUNCTION( sn4_bf_write, GetNumBitsWritten );
		REG_META_FUNCTION( sn4_bf_write, GetNumBytesWritten );
		REG_META_FUNCTION( sn4_bf_write, GetNumBitsLeft );
		REG_META_FUNCTION( sn4_bf_write, GetNumBytesLeft );

		REG_META_FUNCTION( sn4_bf_write, IsOverflowed );

		REG_META_FUNCTION( sn4_bf_write, Seek );

		REG_META_FUNCTION( sn4_bf_write, WriteBitAngle );
		REG_META_FUNCTION( sn4_bf_write, WriteBits );
		REG_META_FUNCTION( sn4_bf_write, WriteBitVec3Coord );
		REG_META_FUNCTION( sn4_bf_write, WriteByte );
		REG_META_FUNCTION( sn4_bf_write, WriteBytes );
		REG_META_FUNCTION( sn4_bf_write, WriteChar );
		REG_META_FUNCTION( sn4_bf_write, WriteFloat );
		REG_META_FUNCTION( sn4_bf_write, WriteLong );
		REG_META_FUNCTION( sn4_bf_write, WriteOneBit );
		REG_META_FUNCTION( sn4_bf_write, WriteShort );
		REG_META_FUNCTION( sn4_bf_write, WriteString );
		REG_META_FUNCTION( sn4_bf_write, WriteSBitLong );
		REG_META_FUNCTION( sn4_bf_write, WriteUBitLong );
		REG_META_FUNCTION( sn4_bf_write, WriteWord );

		REG_META_FUNCTION( sn4_bf_write, FinishWriting );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( sn4_bf_write );

	BEGIN_META_REGISTRATION( sn4_bf_read );
		REG_META_FUNCTION( sn4_bf_read, GetBasePointer );
		REG_META_FUNCTION( sn4_bf_read, GetNumBitsLeft );
		REG_META_FUNCTION( sn4_bf_read, GetNumBytesLeft );
		REG_META_FUNCTION( sn4_bf_read, GetNumBitsRead );

		REG_META_FUNCTION( sn4_bf_read, IsOverflowed );

		REG_META_FUNCTION( sn4_bf_read, ReadBitAngle );
		REG_META_FUNCTION( sn4_bf_read, ReadBitAngles );
		REG_META_FUNCTION( sn4_bf_read, ReadBits );
		REG_META_FUNCTION( sn4_bf_read, ReadBitVec3Coord );
		REG_META_FUNCTION( sn4_bf_read, ReadByte );
		REG_META_FUNCTION( sn4_bf_read, ReadBytes );
		REG_META_FUNCTION( sn4_bf_read, ReadChar );
		REG_META_FUNCTION( sn4_bf_read, ReadFloat );
		REG_META_FUNCTION( sn4_bf_read, ReadLong );
		REG_META_FUNCTION( sn4_bf_read, ReadOneBit );
		REG_META_FUNCTION( sn4_bf_read, ReadShort );
		REG_META_FUNCTION( sn4_bf_read, ReadString );
		REG_META_FUNCTION( sn4_bf_read, ReadSBitLong );
		REG_META_FUNCTION( sn4_bf_read, ReadUBitLong );
		REG_META_FUNCTION( sn4_bf_read, ReadWord );

		REG_META_FUNCTION( sn4_bf_read, Seek );
		REG_META_FUNCTION( sn4_bf_read, SeekRelative );

		REG_META_FUNCTION( sn4_bf_read, TotalBytesAvailable );

		REG_META_FUNCTION( sn4_bf_read, FinishReading );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( sn4_bf_read );

	BEGIN_META_REGISTRATION( CNetChan );
		REG_META_CALLBACK( CNetChan, __eq );

		REG_META_FUNCTION( CNetChan, DumpMessages );

		REG_META_FUNCTION( CNetChan, Reset );
		REG_META_FUNCTION( CNetChan, Clear );
		REG_META_FUNCTION( CNetChan, Shutdown );
		REG_META_FUNCTION( CNetChan, Transmit );

		REG_META_FUNCTION( CNetChan, SendFile );
		REG_META_FUNCTION( CNetChan, DenyFile );
		REG_META_FUNCTION( CNetChan, RequestFile );

		REG_META_FUNCTION( CNetChan, GetOutgoingQueueSize );
		REG_META_FUNCTION( CNetChan, GetOutgoingQueueFragments );
		REG_META_FUNCTION( CNetChan, QueueOutgoingFragments );

		REG_META_FUNCTION( CNetChan, GetIncomingFragments );

		REG_META_FUNCTION( CNetChan, GetSubChannels );

		REG_META_FUNCTION( CNetChan, GetReliableBuffer );
		REG_META_FUNCTION( CNetChan, GetUnreliableBuffer );
		REG_META_FUNCTION( CNetChan, GetVoiceBuffer );

		REG_META_FUNCTION( CNetChan, GetNetChannelHandler );
		REG_META_FUNCTION( CNetChan, GetAddress );
		REG_META_FUNCTION( CNetChan, GetTime );
		REG_META_FUNCTION( CNetChan, GetLatency );
		REG_META_FUNCTION( CNetChan, GetAvgLatency );
		REG_META_FUNCTION( CNetChan, GetAvgLoss );
		REG_META_FUNCTION( CNetChan, GetAvgChoke );
		REG_META_FUNCTION( CNetChan, GetAvgData );
		REG_META_FUNCTION( CNetChan, GetAvgPackets );
		REG_META_FUNCTION( CNetChan, GetTotalData );
		REG_META_FUNCTION( CNetChan, GetSequenceNr );
		REG_META_FUNCTION( CNetChan, IsValidPacket );
		REG_META_FUNCTION( CNetChan, GetPacketTime );
		REG_META_FUNCTION( CNetChan, GetPacketBytes );
		REG_META_FUNCTION( CNetChan, GetStreamProgress );
		REG_META_FUNCTION( CNetChan, GetCommandInterpolationAmount );
		REG_META_FUNCTION( CNetChan, GetPacketResponseLatency );
		REG_META_FUNCTION( CNetChan, GetRemoteFramerate );

		REG_META_FUNCTION( CNetChan, SetInterpolationAmount );
		REG_META_FUNCTION( CNetChan, SetRemoteFramerate );
		REG_META_FUNCTION( CNetChan, SetMaxBufferSize );

		REG_META_FUNCTION( CNetChan, IsPlayback );

		REG_META_FUNCTION( CNetChan, GetTimeoutSeconds );
		REG_META_FUNCTION( CNetChan, SetTimeoutSeconds );

		REG_META_FUNCTION( CNetChan, GetConnectTime );
		REG_META_FUNCTION( CNetChan, SetConnectTime );

		REG_META_FUNCTION( CNetChan, GetLastReceivedTime );
		REG_META_FUNCTION( CNetChan, SetLastReceivedTime );

		REG_META_FUNCTION( CNetChan, GetName );
		REG_META_FUNCTION( CNetChan, SetName );

		REG_META_FUNCTION( CNetChan, GetRate );
		REG_META_FUNCTION( CNetChan, SetRate );

		REG_META_FUNCTION( CNetChan, GetBackgroundMode );
		REG_META_FUNCTION( CNetChan, SetBackgroundMode );

		REG_META_FUNCTION( CNetChan, GetCompressionMode );
		REG_META_FUNCTION( CNetChan, SetCompressionMode );

		REG_META_FUNCTION( CNetChan, GetMaxRoutablePayloadSize );
		REG_META_FUNCTION( CNetChan, SetMaxRoutablePayloadSize );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( CNetChan );

	BEGIN_META_REGISTRATION( subchannel_t );
		REG_META_FUNCTION( subchannel_t, GetFragmentOffset );
		REG_META_FUNCTION( subchannel_t, SetFragmentOffset );

		REG_META_FUNCTION( subchannel_t, GetFragmentNumber );
		REG_META_FUNCTION( subchannel_t, SetFragmentNumber );

		REG_META_FUNCTION( subchannel_t, GetSequence );
		REG_META_FUNCTION( subchannel_t, SetSequence );

		REG_META_FUNCTION( subchannel_t, GetState );
		REG_META_FUNCTION( subchannel_t, SetState );

		REG_META_FUNCTION( subchannel_t, GetIndex );
		REG_META_FUNCTION( subchannel_t, SetIndex );
	END_META_REGISTRATION( );

	BEGIN_META_REGISTRATION( dataFragments_t );
		REG_META_FUNCTION( dataFragments_t, GetFileHandle );
		REG_META_FUNCTION( dataFragments_t, SetFileHandle );

		REG_META_FUNCTION( dataFragments_t, GetFileName );
		REG_META_FUNCTION( dataFragments_t, SetFileName );

		REG_META_FUNCTION( dataFragments_t, GetFileTransferID );
		REG_META_FUNCTION( dataFragments_t, SetFileTransferID );

		REG_META_FUNCTION( dataFragments_t, GetBuffer );
		REG_META_FUNCTION( dataFragments_t, SetBuffer );

		REG_META_FUNCTION( dataFragments_t, GetBytes );
		REG_META_FUNCTION( dataFragments_t, SetBytes );

		REG_META_FUNCTION( dataFragments_t, GetBits );
		REG_META_FUNCTION( dataFragments_t, SetBits );

		REG_META_FUNCTION( dataFragments_t, GetActualSize );
		REG_META_FUNCTION( dataFragments_t, SetActualSize );

		REG_META_FUNCTION( dataFragments_t, GetCompressed );
		REG_META_FUNCTION( dataFragments_t, SetCompressed );

		REG_META_FUNCTION( dataFragments_t, GetStream );
		REG_META_FUNCTION( dataFragments_t, SetStream );

		REG_META_FUNCTION( dataFragments_t, GetTotal );
		REG_META_FUNCTION( dataFragments_t, SetTotal );

		REG_META_FUNCTION( dataFragments_t, GetProgress );
		REG_META_FUNCTION( dataFragments_t, SetProgress );

		REG_META_FUNCTION( dataFragments_t, GetNum );
		REG_META_FUNCTION( dataFragments_t, SetNum );

		REG_META_FUNCTION( dataFragments_t, Delete );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( dataFragments_t );

	BEGIN_META_REGISTRATION( FileHandle_t );
	END_META_REGISTRATION( );

	BEGIN_META_REGISTRATION( UCHARPTR );
		REG_META_FUNCTION( UCHARPTR, Delete );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( UCHARPTR );

	BEGIN_META_REGISTRATION( netadr_t );
		REG_META_FUNCTION( netadr_t, IsLocalhost );
		REG_META_FUNCTION( netadr_t, IsLoopback );
		REG_META_FUNCTION( netadr_t, IsReservedAdr );
		REG_META_FUNCTION( netadr_t, IsValid );

		REG_META_FUNCTION( netadr_t, GetIP );
		REG_META_FUNCTION( netadr_t, GetPort );
		REG_META_FUNCTION( netadr_t, GetType );

		REG_META_FUNCTION( netadr_t, ToString );
	END_META_REGISTRATION( );

	BEGIN_META_REGISTRATION( INetworkStringTableContainer );
		REG_META_FUNCTION( INetworkStringTableContainer, FindTable );
		REG_META_FUNCTION( INetworkStringTableContainer, GetTable );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( INetworkStringTableContainer );

	BEGIN_META_REGISTRATION( INetworkStringTable );
		REG_META_FUNCTION( INetworkStringTable, GetString );
	END_META_REGISTRATION( );

	BEGIN_META_REGISTRATION( IGameEventManager2 );
		REG_META_FUNCTION( IGameEventManager2, CreateEvent );
		REG_META_FUNCTION( IGameEventManager2, SerializeEvent );
		REG_META_FUNCTION( IGameEventManager2, UnserializeEvent );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( IGameEventManager2 );

	BEGIN_META_REGISTRATION( IGameEvent );
		REG_META_FUNCTION( IGameEvent, GetName );

		REG_META_FUNCTION( IGameEvent, IsReliable );
		REG_META_FUNCTION( IGameEvent, IsLocal );
		REG_META_FUNCTION( IGameEvent, IsEmpty );

		REG_META_FUNCTION( IGameEvent, GetBool );
		REG_META_FUNCTION( IGameEvent, GetInt );
		REG_META_FUNCTION( IGameEvent, GetFloat );
		REG_META_FUNCTION( IGameEvent, GetString );

		REG_META_FUNCTION( IGameEvent, SetBool );
		REG_META_FUNCTION( IGameEvent, SetInt );
		REG_META_FUNCTION( IGameEvent, SetFloat );
		REG_META_FUNCTION( IGameEvent, SetString );

		REG_META_FUNCTION( IGameEvent, Delete );
	END_META_REGISTRATION( );

	REG_GLBL_FUNCTION( Attach__CNetChan_ProcessPacket );
	REG_GLBL_FUNCTION( Detach__CNetChan_ProcessPacket );

	REG_GLBL_FUNCTION( Attach__CNetChan_SendDatagram );
	REG_GLBL_FUNCTION( Detach__CNetChan_SendDatagram );

	REG_GLBL_FUNCTION( Attach__CNetChan_Shutdown );
	REG_GLBL_FUNCTION( Detach__CNetChan_Shutdown );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_ConnectionStart );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_ConnectionStart );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_ConnectionClosing );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_ConnectionClosing );
	
	REG_GLBL_FUNCTION( Attach__INetChannelHandler_ConnectionCrashed );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_ConnectionCrashed );
	
	REG_GLBL_FUNCTION( Attach__INetChannelHandler_PacketStart );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_PacketStart );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_PacketEnd );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_PacketEnd );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_FileRequested );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_FileRequested );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_FileReceived );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_FileReceived );

	REG_GLBL_FUNCTION( Attach__INetChannelHandler_FileDenied );
	REG_GLBL_FUNCTION( Detach__INetChannelHandler_FileDenied );

	REG_GLBL_FUNCTION( Attach__CNetChan_ProcessMessages );
	REG_GLBL_FUNCTION( Detach__CNetChan_ProcessMessages );

	REG_GLBL_FUNCTION( sourcenet_isDedicatedServer );

	if( !IS_ATTACHED( CNetChan_ProcessMessages ) )
	{
		CNetChan_ProcessMessages_O = static_cast<CNetChan_ProcessMessages_T>(
			symfinder.ResolveOnBinary( engine_lib, CNetChan_ProcessMessages_sig, CNetChan_ProcessMessages_siglen )
		);
		if( CNetChan_ProcessMessages_O == nullptr )
			LUA->ThrowError( "failed to locate CNetChan::ProcessMessages, report this!" );
	}
	
#if defined SOURCENET_SERVER

	// Disables per-client threads (hacky fix for SendDatagram hooking)

	uint8_t *ulNetThreadChunk = static_cast<uint8_t *>( symfinder.ResolveOnBinary( engine_lib, netchunk_sig, netchunk_siglen ) );
	if( ulNetThreadChunk != nullptr )
	{
		ulNetThreadChunk += netchunk_sig_offset;

		BEGIN_MEMEDIT( ulNetThreadChunk, netpatch_len );
			memcpy( ulNetThreadChunk, netpatch_new, netpatch_len );
		FINISH_MEMEDIT( ulNetThreadChunk, netpatch_len );

		g_bPatchedNetChunk = true;
	}
	else
	{
		g_bPatchedNetChunk = false;

		LUA->ThrowError( "failed to locate net thread chunk, report this!" );
	}

#endif

	return 0;
}

// Module shutdown
GMOD_MODULE_CLOSE( )
{

#if defined SOURCENET_SERVER

	if( g_bPatchedNetChunk )
	{
		SymbolFinder symfinder;

		uint8_t *ulNetThreadChunk = static_cast<uint8_t *>( symfinder.ResolveOnBinary( engine_lib, netchunk_sig, netchunk_siglen ) );
		if( ulNetThreadChunk != nullptr )
		{
			ulNetThreadChunk += netchunk_sig_offset;

			BEGIN_MEMEDIT( ulNetThreadChunk, netpatch_len );
				memcpy( ulNetThreadChunk, netpatch_old, netpatch_len );
			FINISH_MEMEDIT( ulNetThreadChunk, netpatch_len );
		}
	}

#endif

	return 0;
}