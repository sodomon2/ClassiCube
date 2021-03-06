#ifndef CC_SERVERCONNECTION_H
#define CC_SERVERCONNECTION_H
#include "Input.h"
#include "Vectors.h"
/* Represents a connection to either a multiplayer or an internal singleplayer server.
   Copyright 2014-2019 ClassiCube | Licensed under BSD-3
*/

struct PickedPos;
struct Stream;
struct IGameComponent;
struct ScheduledTask;
extern struct IGameComponent Server_Component;

/* Prepares a ping entry for sending to the server, then returns its ID. */
int Ping_NextPingId(void);
/* Updates received time for ping entry with matching ID. */
void Ping_Update(int id);
/* Calculates average ping time based on most recent ping entries. */
int Ping_AveragePingMS(void);

/* Data for currently active connection to a server. */
CC_VAR extern struct _ServerConnectionData {
	/* Begins connecting to the server. */
	/* NOTE: Usually asynchronous, but not always. */
	void (*BeginConnect)(void);
	/* Ticks state of the server. */
	void (*Tick)(struct ScheduledTask* task);
	/* Sends a block update to the server. */
	void (*SendBlock)(int x, int y, int z, BlockID old, BlockID now);
	/* Sends a chat message to the server. */
	void (*SendChat)(const String* text);
	/* Sends a position update to the server. */
	void (*SendPosition)(Vec3 pos, float yaw, float pitch);
	/* Sends raw data to the server. */
	/* NOTE: Prefer SendBlock/Position/Chat instead, this does NOT work in singleplayer. */
	void (*SendData)(const cc_uint8* data, cc_uint32 len);

	/* The current name of the server. (Shows as first line when loading) */
	String Name;
	/* The current MOTD of the server. (Shows as second line when loading) */
	String MOTD;
	/* The software name the client identifies itself as being to the server. */
	/* By default this is GAME_APP_NAME. */
	String AppName;

	/* Buffer to data to send to the server. */
	cc_uint8* WriteBuffer;
	/* Whether the player is connected to singleplayer/internal server. */
	cc_bool IsSinglePlayer;
	/* Whether the player has been disconnected from the server. */
	cc_bool Disconnected;

	/* Whether the server supports separate tab list from entities in world. */
	cc_bool SupportsExtPlayerList;
	/* Whether the server supports packet with detailed info on mouse clicks. */
	cc_bool SupportsPlayerClick;
	/* Whether the server supports combining multiple chat packets into one. */
	cc_bool SupportsPartialMessages;
	/* Whether the server supports all of code page 437, not just ASCII. */
	cc_bool SupportsFullCP437;

	/* IP address of the server if multiplayer, empty string if singleplayer. */
	String IP;
	/* Port of the server if multiplayer, 0 if singleplayer. */
	int Port;
} Server;

/* If user hasn't previously accepted url, displays a dialog asking to confirm downloading it. */
/* Otherwise just calls World_ApplyTexturePack. */
void Server_RetrieveTexturePack(const String* url);
void Net_SendPacket(void);
#endif
