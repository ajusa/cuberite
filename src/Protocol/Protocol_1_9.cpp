
// Protocol_1_9.cpp

/*
Implements the 1.9 protocol classes:
	- cProtocol_1_9_0
		- release 1.9 protocol (#107)
	- cProtocol_1_9_1
		- release 1.9.1 protocol (#108)
	- cProtocol_1_9_2
		- release 1.9.2 protocol (#109)
	- cProtocol_1_9_4
		- release 1.9.4 protocol (#110)
*/

#include "Globals.h"
#include "json/json.h"
#include "Protocol_1_9.h"
#include "ChunkDataSerializer.h"
#include "../mbedTLS++/Sha1Checksum.h"
#include "Packetizer.h"

#include "../ClientHandle.h"
#include "../Root.h"
#include "../Server.h"
#include "../World.h"
#include "../StringCompression.h"
#include "../CompositeChat.h"
#include "../Statistics.h"

#include "../WorldStorage/FastNBT.h"

#include "../Entities/ExpOrb.h"
#include "../Entities/Minecart.h"
#include "../Entities/FallingBlock.h"
#include "../Entities/Painting.h"
#include "../Entities/Pickup.h"
#include "../Entities/Player.h"
#include "../Entities/ItemFrame.h"
#include "../Entities/ArrowEntity.h"
#include "../Entities/FireworkEntity.h"
#include "../Entities/SplashPotionEntity.h"

#include "../Items/ItemSpawnEgg.h"

#include "../Mobs/IncludeAllMonsters.h"
#include "../UI/HorseWindow.h"

#include "../BlockEntities/BeaconEntity.h"
#include "../BlockEntities/CommandBlockEntity.h"
#include "../BlockEntities/MobHeadEntity.h"
#include "../BlockEntities/MobSpawnerEntity.h"
#include "../BlockEntities/FlowerPotEntity.h"
#include "../Bindings/PluginManager.h"





/** The slot number that the client uses to indicate "outside the window". */
static const Int16 SLOT_NUM_OUTSIDE = -999;

/** Value for main hand in Hand parameter for Protocol 1.9. */
static const UInt32 MAIN_HAND = 0;
static const UInt32 OFF_HAND = 1;





#define HANDLE_READ(ByteBuf, Proc, Type, Var) \
	Type Var; \
	do { \
		if (!ByteBuf.Proc(Var))\
		{\
			return;\
		} \
	} while (false)





#define HANDLE_PACKET_READ(ByteBuf, Proc, Type, Var) \
	Type Var; \
	do { \
		{ \
			if (!ByteBuf.Proc(Var)) \
			{ \
				ByteBuf.CheckValid(); \
				return false; \
			} \
			ByteBuf.CheckValid(); \
		} \
	} while (false)





const int MAX_ENC_LEN = 512;  // Maximum size of the encrypted message; should be 128, but who knows...
const uLongf MAX_COMPRESSED_PACKET_LEN = 200 KiB;  // Maximum size of compressed packets.





// fwd: main.cpp:
extern bool g_ShouldLogCommIn, g_ShouldLogCommOut;





////////////////////////////////////////////////////////////////////////////////
// cProtocol_1_9_0:

cProtocol_1_9_0::cProtocol_1_9_0(cClientHandle * a_Client, const AString & a_ServerAddress, UInt16 a_ServerPort, UInt32 a_State) :
	Super(a_Client),
	m_ServerAddress(a_ServerAddress),
	m_ServerPort(a_ServerPort),
	m_State(a_State),
	m_IsTeleportIdConfirmed(true),
	m_OutstandingTeleportId(0),
	m_ReceivedData(32 KiB),
	m_IsEncrypted(false)
{

	AStringVector Params;
	SplitZeroTerminatedStrings(a_ServerAddress, Params);

	if (Params.size() >= 2)
	{
		m_ServerAddress = Params[0];

		if (Params[1] == "FML")
		{
			LOGD("Forge client connected!");
			m_Client->SetIsForgeClient();
		}
		else if (Params.size() == 4)
		{
			if (cRoot::Get()->GetServer()->ShouldAllowBungeeCord())
			{
				// BungeeCord handling:
				// If BC is setup with ip_forward == true, it sends additional data in the login packet's ServerAddress field:
				// hostname\00ip-address\00uuid\00profile-properties-as-json

				LOGD("Player at %s connected via BungeeCord", Params[1].c_str());

				m_Client->SetIPString(Params[1]);

				cUUID UUID;
				UUID.FromString(Params[2]);
				m_Client->SetUUID(UUID);

				Json::Value root;
				Json::Reader reader;
				if (!reader.parse(Params[3], root))
				{
					LOGERROR("Unable to parse player properties: '%s'", Params[3]);
				}
				else
				{
					m_Client->SetProperties(root);
				}
			}
			else
			{
				LOG("BungeeCord is disabled, but client sent additional data, set AllowBungeeCord=1 if you want to allow it");
			}
		}
		else
		{
			LOG("Unknown additional data sent in server address (BungeeCord/FML?): %zu parameters", Params.size());
			// TODO: support FML + BungeeCord? (what parameters does it send in that case?) https://github.com/SpigotMC/BungeeCord/issues/899
		}
	}

	// Create the comm log file, if so requested:
	if (g_ShouldLogCommIn || g_ShouldLogCommOut)
	{
		static int sCounter = 0;
		cFile::CreateFolder("CommLogs");
		AString IP(a_Client->GetIPString());
		ReplaceString(IP, ":", "_");
		AString FileName = Printf("CommLogs/%x_%d__%s.log",
			static_cast<unsigned>(time(nullptr)),
			sCounter++,
			IP.c_str()
		);
		if (!m_CommLogFile.Open(FileName, cFile::fmWrite))
		{
			LOG("Cannot log communication to file, the log file \"%s\" cannot be opened for writing.", FileName.c_str());
		}
	}
}





void cProtocol_1_9_0::DataReceived(const char * a_Data, size_t a_Size)
{
	if (m_IsEncrypted)
	{
		Byte Decrypted[512];
		while (a_Size > 0)
		{
			size_t NumBytes = (a_Size > sizeof(Decrypted)) ? sizeof(Decrypted) : a_Size;
			m_Decryptor.ProcessData(Decrypted, reinterpret_cast<const Byte *>(a_Data), NumBytes);
			AddReceivedData(reinterpret_cast<const char *>(Decrypted), NumBytes);
			a_Size -= NumBytes;
			a_Data += NumBytes;
		}
	}
	else
	{
		AddReceivedData(a_Data, a_Size);
	}
}





void cProtocol_1_9_0::SendAttachEntity(const cEntity & a_Entity, const cEntity & a_Vehicle)
{
	ASSERT(m_State == 3);  // In game mode?
	cPacketizer Pkt(*this, pktAttachEntity);
	Pkt.WriteVarInt32(a_Vehicle.GetUniqueID());
	Pkt.WriteVarInt32(1);  // 1 passenger
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
}





void cProtocol_1_9_0::SendBlockAction(int a_BlockX, int a_BlockY, int a_BlockZ, char a_Byte1, char a_Byte2, BLOCKTYPE a_BlockType)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktBlockAction);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
	Pkt.WriteBEInt8(a_Byte1);
	Pkt.WriteBEInt8(a_Byte2);
	Pkt.WriteVarInt32(a_BlockType);
}





void cProtocol_1_9_0::SendBlockBreakAnim(UInt32 a_EntityID, int a_BlockX, int a_BlockY, int a_BlockZ, char a_Stage)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktBlockBreakAnim);
	Pkt.WriteVarInt32(a_EntityID);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
	Pkt.WriteBEInt8(a_Stage);
}





void cProtocol_1_9_0::SendBlockChange(int a_BlockX, int a_BlockY, int a_BlockZ, BLOCKTYPE a_BlockType, NIBBLETYPE a_BlockMeta)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktBlockChange);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
	Pkt.WriteVarInt32((static_cast<UInt32>(a_BlockType) << 4) | (static_cast<UInt32>(a_BlockMeta) & 15));
}





void cProtocol_1_9_0::SendBlockChanges(int a_ChunkX, int a_ChunkZ, const sSetBlockVector & a_Changes)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktBlockChanges);
	Pkt.WriteBEInt32(a_ChunkX);
	Pkt.WriteBEInt32(a_ChunkZ);
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Changes.size()));
	for (sSetBlockVector::const_iterator itr = a_Changes.begin(), end = a_Changes.end(); itr != end; ++itr)
	{
		Int16 Coords = static_cast<Int16>(itr->m_RelY | (itr->m_RelZ << 8) | (itr->m_RelX << 12));
		Pkt.WriteBEInt16(Coords);
		Pkt.WriteVarInt32(static_cast<UInt32>(itr->m_BlockType & 0xFFF) << 4 | (itr->m_BlockMeta & 0xF));
	}  // for itr - a_Changes[]
}





void cProtocol_1_9_0::SendCameraSetTo(const cEntity & a_Entity)
{
	cPacketizer Pkt(*this, pktCameraSetTo);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
}





void cProtocol_1_9_0::SendChat(const AString & a_Message, eChatType a_Type)
{
	ASSERT(m_State == 3);  // In game mode?

	SendChatRaw(Printf("{\"text\":\"%s\"}", EscapeString(a_Message).c_str()), a_Type);
}





void cProtocol_1_9_0::SendChat(const cCompositeChat & a_Message, eChatType a_Type, bool a_ShouldUseChatPrefixes)
{
	ASSERT(m_State == 3);  // In game mode?

	SendChatRaw(a_Message.CreateJsonString(a_ShouldUseChatPrefixes), a_Type);
}





void cProtocol_1_9_0::SendChatRaw(const AString & a_MessageRaw, eChatType a_Type)
{
	ASSERT(m_State == 3);  // In game mode?

	// Send the json string to the client:
	cPacketizer Pkt(*this, pktChatRaw);
	Pkt.WriteString(a_MessageRaw);
	Pkt.WriteBEInt8(a_Type);
}





void cProtocol_1_9_0::SendChunkData(int a_ChunkX, int a_ChunkZ, cChunkDataSerializer & a_Serializer)
{
	ASSERT(m_State == 3);  // In game mode?

	// Serialize first, before creating the Packetizer (the packetizer locks a CS)
	// This contains the flags and bitmasks, too
	const AString & ChunkData = a_Serializer.Serialize(cChunkDataSerializer::RELEASE_1_9_0, a_ChunkX, a_ChunkZ);

	cCSLock Lock(m_CSPacket);
	SendData(ChunkData.data(), ChunkData.size());
}





void cProtocol_1_9_0::SendCollectEntity(const cEntity & a_Entity, const cPlayer & a_Player, int a_Count)
{
	UNUSED(a_Count);
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktCollectEntity);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteVarInt32(a_Player.GetUniqueID());
}





void cProtocol_1_9_0::SendDestroyEntity(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktDestroyEntity);
	Pkt.WriteVarInt32(1);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
}





void cProtocol_1_9_0::SendDetachEntity(const cEntity & a_Entity, const cEntity & a_PreviousVehicle)
{
	ASSERT(m_State == 3);  // In game mode?
	cPacketizer Pkt(*this, pktAttachEntity);
	Pkt.WriteVarInt32(a_PreviousVehicle.GetUniqueID());
	Pkt.WriteVarInt32(0);  // No passangers
}





void cProtocol_1_9_0::SendDisconnect(const AString & a_Reason)
{
	switch (m_State)
	{
		case 2:
		{
			// During login:
			cPacketizer Pkt(*this, pktDisconnectDuringLogin);
			Pkt.WriteString(Printf("{\"text\":\"%s\"}", EscapeString(a_Reason).c_str()));
			break;
		}
		case 3:
		{
			// In-game:
			cPacketizer Pkt(*this, pktDisconnectDuringGame);
			Pkt.WriteString(Printf("{\"text\":\"%s\"}", EscapeString(a_Reason).c_str()));
			break;
		}
	}
}





void cProtocol_1_9_0::SendEditSign(int a_BlockX, int a_BlockY, int a_BlockZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEditSign);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
}





void cProtocol_1_9_0::SendEntityEffect(const cEntity & a_Entity, int a_EffectID, int a_Amplifier, int a_Duration)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityEffect);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_EffectID));
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_Amplifier));
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Duration));
	Pkt.WriteBool(false);  // Hide particles
}





void cProtocol_1_9_0::SendEntityEquipment(const cEntity & a_Entity, short a_SlotNum, const cItem & a_Item)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityEquipment);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	// Needs to be adjusted due to the insertion of offhand at slot 1
	if (a_SlotNum > 0)
	{
		a_SlotNum++;
	}
	Pkt.WriteVarInt32(static_cast<UInt32>(a_SlotNum));
	WriteItem(Pkt, a_Item);
}





void cProtocol_1_9_0::SendEntityHeadLook(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityHeadLook);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteByteAngle(a_Entity.GetHeadYaw());
}





void cProtocol_1_9_0::SendEntityLook(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityLook);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteByteAngle(a_Entity.GetYaw());
	Pkt.WriteByteAngle(a_Entity.GetPitch());
	Pkt.WriteBool(a_Entity.IsOnGround());
}





void cProtocol_1_9_0::SendEntityMetadata(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityMeta);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	WriteEntityMetadata(Pkt, a_Entity);
	Pkt.WriteBEUInt8(0xff);  // The termination byte
}





void cProtocol_1_9_0::SendEntityProperties(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityProperties);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	WriteEntityProperties(Pkt, a_Entity);
}





void cProtocol_1_9_0::SendEntityRelMove(const cEntity & a_Entity, char a_RelX, char a_RelY, char a_RelZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityRelMove);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	// TODO: 1.9 changed these from chars to shorts, meaning that there can be more percision and data.  Other code needs to be updated for that.
	Pkt.WriteBEInt16(a_RelX * 128);
	Pkt.WriteBEInt16(a_RelY * 128);
	Pkt.WriteBEInt16(a_RelZ * 128);
	Pkt.WriteBool(a_Entity.IsOnGround());
}





void cProtocol_1_9_0::SendEntityRelMoveLook(const cEntity & a_Entity, char a_RelX, char a_RelY, char a_RelZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityRelMoveLook);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	// TODO: 1.9 changed these from chars to shorts, meaning that there can be more percision and data.  Other code needs to be updated for that.
	Pkt.WriteBEInt16(a_RelX * 128);
	Pkt.WriteBEInt16(a_RelY * 128);
	Pkt.WriteBEInt16(a_RelZ * 128);
	Pkt.WriteByteAngle(a_Entity.GetYaw());
	Pkt.WriteByteAngle(a_Entity.GetPitch());
	Pkt.WriteBool(a_Entity.IsOnGround());
}





void cProtocol_1_9_0::SendEntityStatus(const cEntity & a_Entity, char a_Status)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityStatus);
	Pkt.WriteBEUInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEInt8(a_Status);
}





void cProtocol_1_9_0::SendEntityVelocity(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityVelocity);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	// 400 = 8000 / 20 ... Conversion from our speed in m / s to 8000 m / tick
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedX() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedY() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedZ() * 400));
}





void cProtocol_1_9_0::SendExperience(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktExperience);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteBEFloat(Player->GetXpPercentage());
	Pkt.WriteVarInt32(static_cast<UInt32>(Player->GetXpLevel()));
	Pkt.WriteVarInt32(static_cast<UInt32>(Player->GetCurrentXp()));
}





void cProtocol_1_9_0::SendExperienceOrb(const cExpOrb & a_ExpOrb)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSpawnExperienceOrb);
	Pkt.WriteVarInt32(a_ExpOrb.GetUniqueID());
	Pkt.WriteBEDouble(a_ExpOrb.GetPosX());
	Pkt.WriteBEDouble(a_ExpOrb.GetPosY());
	Pkt.WriteBEDouble(a_ExpOrb.GetPosZ());
	Pkt.WriteBEInt16(static_cast<Int16>(a_ExpOrb.GetReward()));
}





void cProtocol_1_9_0::SendExplosion(double a_BlockX, double a_BlockY, double a_BlockZ, float a_Radius, const cVector3iArray & a_BlocksAffected, const Vector3d & a_PlayerMotion)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktExplosion);
	Pkt.WriteBEFloat(static_cast<float>(a_BlockX));
	Pkt.WriteBEFloat(static_cast<float>(a_BlockY));
	Pkt.WriteBEFloat(static_cast<float>(a_BlockZ));
	Pkt.WriteBEFloat(static_cast<float>(a_Radius));
	Pkt.WriteBEUInt32(static_cast<UInt32>(a_BlocksAffected.size()));
	for (cVector3iArray::const_iterator itr = a_BlocksAffected.begin(), end = a_BlocksAffected.end(); itr != end; ++itr)
	{
		Pkt.WriteBEInt8(static_cast<Int8>(itr->x));
		Pkt.WriteBEInt8(static_cast<Int8>(itr->y));
		Pkt.WriteBEInt8(static_cast<Int8>(itr->z));
	}  // for itr - a_BlockAffected[]
	Pkt.WriteBEFloat(static_cast<float>(a_PlayerMotion.x));
	Pkt.WriteBEFloat(static_cast<float>(a_PlayerMotion.y));
	Pkt.WriteBEFloat(static_cast<float>(a_PlayerMotion.z));
}





void cProtocol_1_9_0::SendGameMode(eGameMode a_GameMode)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktGameMode);
	Pkt.WriteBEUInt8(3);  // Reason: Change game mode
	Pkt.WriteBEFloat(static_cast<float>(a_GameMode));  // The protocol really represents the value with a float!
}





void cProtocol_1_9_0::SendHealth(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUpdateHealth);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteBEFloat(static_cast<float>(Player->GetHealth()));
	Pkt.WriteVarInt32(static_cast<UInt32>(Player->GetFoodLevel()));
	Pkt.WriteBEFloat(static_cast<float>(Player->GetFoodSaturationLevel()));
}





void cProtocol_1_9_0::SendHeldItemChange(int a_ItemIndex)
{
	ASSERT((a_ItemIndex >= 0) && (a_ItemIndex <= 8));  // Valid check

	cPacketizer Pkt(*this, pktHeldItemChange);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteBEInt8(static_cast<Int8>(Player->GetInventory().GetEquippedSlotNum()));
}





void cProtocol_1_9_0::SendHideTitle(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTitle);
	Pkt.WriteVarInt32(3);  // Hide title
}





void cProtocol_1_9_0::SendInventorySlot(char a_WindowID, short a_SlotNum, const cItem & a_Item)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktInventorySlot);
	Pkt.WriteBEInt8(a_WindowID);
	Pkt.WriteBEInt16(a_SlotNum);
	WriteItem(Pkt, a_Item);
}





void cProtocol_1_9_0::SendKeepAlive(UInt32 a_PingID)
{
	// Drop the packet if the protocol is not in the Game state yet (caused a client crash):
	if (m_State != 3)
	{
		LOG("Trying to send a KeepAlive packet to a player who's not yet fully logged in (%d). The protocol class prevented the packet.", m_State);
		return;
	}

	cPacketizer Pkt(*this, pktKeepAlive);
	Pkt.WriteVarInt32(a_PingID);
}





void cProtocol_1_9_0::SendLeashEntity(const cEntity & a_Entity, const cEntity & a_EntityLeashedTo)
{
	ASSERT(m_State == 3);  // In game mode?
	cPacketizer Pkt(*this, pktLeashEntity);
	Pkt.WriteBEUInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEUInt32(a_EntityLeashedTo.GetUniqueID());
}





void cProtocol_1_9_0::SendUnleashEntity(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?
	cPacketizer Pkt(*this, pktLeashEntity);
	Pkt.WriteBEUInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEInt32(-1);  // Unleash a_Entity
}





void cProtocol_1_9_0::SendLogin(const cPlayer & a_Player, const cWorld & a_World)
{
	// Send the Join Game packet:
	{
		cServer * Server = cRoot::Get()->GetServer();
		cPacketizer Pkt(*this, pktJoinGame);
		Pkt.WriteBEUInt32(a_Player.GetUniqueID());
		Pkt.WriteBEUInt8(static_cast<UInt8>(a_Player.GetEffectiveGameMode()) | (Server->IsHardcore() ? 0x08 : 0));  // Hardcore flag bit 4
		Pkt.WriteBEInt8(static_cast<Int8>(a_World.GetDimension()));
		Pkt.WriteBEUInt8(2);  // TODO: Difficulty (set to Normal)
		Pkt.WriteBEUInt8(static_cast<UInt8>(Clamp<size_t>(Server->GetMaxPlayers(), 0, 255)));
		Pkt.WriteString("default");  // Level type - wtf?
		Pkt.WriteBool(false);  // Reduced Debug Info - wtf?
	}

	// Send the spawn position:
	{
		cPacketizer Pkt(*this, pktSpawnPosition);
		Pkt.WritePosition64(FloorC(a_World.GetSpawnX()), FloorC(a_World.GetSpawnY()), FloorC(a_World.GetSpawnZ()));
	}

	// Send the server difficulty:
	{
		cPacketizer Pkt(*this, pktDifficulty);
		Pkt.WriteBEInt8(1);
	}

	// Send player abilities:
	SendPlayerAbilities();
}





void cProtocol_1_9_0::SendLoginSuccess(void)
{
	ASSERT(m_State == 2);  // State: login?

	// Enable compression:
	{
		cPacketizer Pkt(*this, pktStartCompression);
		Pkt.WriteVarInt32(256);
	}

	m_State = 3;  // State = Game

	{
		cPacketizer Pkt(*this, pktLoginSuccess);
		Pkt.WriteString(m_Client->GetUUID().ToLongString());
		Pkt.WriteString(m_Client->GetUsername());
	}
}





void cProtocol_1_9_0::SendPaintingSpawn(const cPainting & a_Painting)
{
	ASSERT(m_State == 3);  // In game mode?
	double PosX = a_Painting.GetPosX();
	double PosY = a_Painting.GetPosY();
	double PosZ = a_Painting.GetPosZ();

	cPacketizer Pkt(*this, pktSpawnPainting);
	Pkt.WriteVarInt32(a_Painting.GetUniqueID());
	// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
	Pkt.WriteBEUInt64(0);
	Pkt.WriteBEUInt64(a_Painting.GetUniqueID());
	Pkt.WriteString(a_Painting.GetName().c_str());
	Pkt.WritePosition64(static_cast<Int32>(PosX), static_cast<Int32>(PosY), static_cast<Int32>(PosZ));
	Pkt.WriteBEInt8(static_cast<Int8>(a_Painting.GetProtocolFacing()));
}





void cProtocol_1_9_0::SendMapData(const cMap & a_Map, int a_DataStartX, int a_DataStartY)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktMapData);
	Pkt.WriteVarInt32(a_Map.GetID());
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_Map.GetScale()));

	Pkt.WriteBool(true);
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Map.GetDecorators().size()));
	for (const auto & Decorator : a_Map.GetDecorators())
	{
		Pkt.WriteBEUInt8(static_cast<Byte>((static_cast<Int32>(Decorator.GetType()) << 4) | (Decorator.GetRot() & 0xF)));
		Pkt.WriteBEUInt8(static_cast<UInt8>(Decorator.GetPixelX()));
		Pkt.WriteBEUInt8(static_cast<UInt8>(Decorator.GetPixelZ()));
	}

	Pkt.WriteBEUInt8(128);
	Pkt.WriteBEUInt8(128);
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_DataStartX));
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_DataStartY));
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Map.GetData().size()));
	for (auto itr = a_Map.GetData().cbegin(); itr != a_Map.GetData().cend(); ++itr)
	{
		Pkt.WriteBEUInt8(*itr);
	}
}





void cProtocol_1_9_0::SendPickupSpawn(const cPickup & a_Pickup)
{
	ASSERT(m_State == 3);  // In game mode?

	{  // TODO Use SendSpawnObject
		cPacketizer Pkt(*this, pktSpawnObject);
		Pkt.WriteVarInt32(a_Pickup.GetUniqueID());
		// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
		Pkt.WriteBEUInt64(0);
		Pkt.WriteBEUInt64(a_Pickup.GetUniqueID());
		Pkt.WriteBEUInt8(2);  // Type = Pickup
		Pkt.WriteBEDouble(a_Pickup.GetPosX());
		Pkt.WriteBEDouble(a_Pickup.GetPosY());
		Pkt.WriteBEDouble(a_Pickup.GetPosZ());
		Pkt.WriteByteAngle(a_Pickup.GetYaw());
		Pkt.WriteByteAngle(a_Pickup.GetPitch());
		Pkt.WriteBEInt32(0);  // No object data
		Pkt.WriteBEInt16(0);  // No velocity
		Pkt.WriteBEInt16(0);
		Pkt.WriteBEInt16(0);
	}

	SendEntityMetadata(a_Pickup);
}





void cProtocol_1_9_0::SendPlayerAbilities(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerAbilities);
	Byte Flags = 0;
	cPlayer * Player = m_Client->GetPlayer();
	if (Player->IsGameModeCreative())
	{
		Flags |= 0x01;
		Flags |= 0x08;  // Godmode, used for creative
	}
	if (Player->IsFlying())
	{
		Flags |= 0x02;
	}
	if (Player->CanFly())
	{
		Flags |= 0x04;
	}
	Pkt.WriteBEUInt8(Flags);
	Pkt.WriteBEFloat(static_cast<float>(0.05 * Player->GetFlyingMaxSpeed()));
	Pkt.WriteBEFloat(static_cast<float>(0.1 * Player->GetNormalMaxSpeed()));
}





void cProtocol_1_9_0::SendEntityAnimation(const cEntity & a_Entity, char a_Animation)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktEntityAnimation);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEInt8(a_Animation);
}





void cProtocol_1_9_0::SendParticleEffect(const AString & a_ParticleName, float a_SrcX, float a_SrcY, float a_SrcZ, float a_OffsetX, float a_OffsetY, float a_OffsetZ, float a_ParticleData, int a_ParticleAmount)
{
	ASSERT(m_State == 3);  // In game mode?
	int ParticleID = GetParticleID(a_ParticleName);

	cPacketizer Pkt(*this, pktParticleEffect);
	Pkt.WriteBEInt32(ParticleID);
	Pkt.WriteBool(false);
	Pkt.WriteBEFloat(a_SrcX);
	Pkt.WriteBEFloat(a_SrcY);
	Pkt.WriteBEFloat(a_SrcZ);
	Pkt.WriteBEFloat(a_OffsetX);
	Pkt.WriteBEFloat(a_OffsetY);
	Pkt.WriteBEFloat(a_OffsetZ);
	Pkt.WriteBEFloat(a_ParticleData);
	Pkt.WriteBEInt32(a_ParticleAmount);
}





void cProtocol_1_9_0::SendParticleEffect(const AString & a_ParticleName, Vector3f a_Src, Vector3f a_Offset, float a_ParticleData, int a_ParticleAmount, std::array<int, 2> a_Data)
{
	ASSERT(m_State == 3);  // In game mode?
	int ParticleID = GetParticleID(a_ParticleName);

	cPacketizer Pkt(*this, pktParticleEffect);
	Pkt.WriteBEInt32(ParticleID);
	Pkt.WriteBool(false);
	Pkt.WriteBEFloat(a_Src.x);
	Pkt.WriteBEFloat(a_Src.y);
	Pkt.WriteBEFloat(a_Src.z);
	Pkt.WriteBEFloat(a_Offset.x);
	Pkt.WriteBEFloat(a_Offset.y);
	Pkt.WriteBEFloat(a_Offset.z);
	Pkt.WriteBEFloat(a_ParticleData);
	Pkt.WriteBEInt32(a_ParticleAmount);
	switch (ParticleID)
	{
		// iconcrack
		case 36:
		{
			Pkt.WriteVarInt32(static_cast<UInt32>(a_Data[0]));
			Pkt.WriteVarInt32(static_cast<UInt32>(a_Data[1]));
			break;
		}
		// blockcrack
		// blockdust
		case 37:
		case 38:
		{
			Pkt.WriteVarInt32(static_cast<UInt32>(a_Data[0]));
			break;
		}
		default:
		{
			break;
		}
	}
}





void cProtocol_1_9_0::SendPlayerListAddPlayer(const cPlayer & a_Player)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerList);
	Pkt.WriteVarInt32(0);
	Pkt.WriteVarInt32(1);
	Pkt.WriteUUID(a_Player.GetUUID());
	Pkt.WriteString(a_Player.GetPlayerListName());

	const Json::Value & Properties = a_Player.GetClientHandle()->GetProperties();
	Pkt.WriteVarInt32(Properties.size());
	for (auto & Node : Properties)
	{
		Pkt.WriteString(Node.get("name", "").asString());
		Pkt.WriteString(Node.get("value", "").asString());
		AString Signature = Node.get("signature", "").asString();
		if (Signature.empty())
		{
			Pkt.WriteBool(false);
		}
		else
		{
			Pkt.WriteBool(true);
			Pkt.WriteString(Signature);
		}
	}

	Pkt.WriteVarInt32(static_cast<UInt32>(a_Player.GetEffectiveGameMode()));
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Player.GetClientHandle()->GetPing()));
	Pkt.WriteBool(false);
}





void cProtocol_1_9_0::SendPlayerListRemovePlayer(const cPlayer & a_Player)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerList);
	Pkt.WriteVarInt32(4);
	Pkt.WriteVarInt32(1);
	Pkt.WriteUUID(a_Player.GetUUID());
}





void cProtocol_1_9_0::SendPlayerListUpdateGameMode(const cPlayer & a_Player)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerList);
	Pkt.WriteVarInt32(1);
	Pkt.WriteVarInt32(1);
	Pkt.WriteUUID(a_Player.GetUUID());
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Player.GetEffectiveGameMode()));
}





void cProtocol_1_9_0::SendPlayerListUpdatePing(const cPlayer & a_Player)
{
	ASSERT(m_State == 3);  // In game mode?

	auto ClientHandle = a_Player.GetClientHandlePtr();
	if (ClientHandle != nullptr)
	{
		cPacketizer Pkt(*this, pktPlayerList);
		Pkt.WriteVarInt32(2);
		Pkt.WriteVarInt32(1);
		Pkt.WriteUUID(a_Player.GetUUID());
		Pkt.WriteVarInt32(static_cast<UInt32>(ClientHandle->GetPing()));
	}
}





void cProtocol_1_9_0::SendPlayerListUpdateDisplayName(const cPlayer & a_Player, const AString & a_CustomName)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerList);
	Pkt.WriteVarInt32(3);
	Pkt.WriteVarInt32(1);
	Pkt.WriteUUID(a_Player.GetUUID());

	if (a_CustomName.empty())
	{
		Pkt.WriteBool(false);
	}
	else
	{
		Pkt.WriteBool(true);
		Pkt.WriteString(Printf("{\"text\":\"%s\"}", a_CustomName.c_str()));
	}
}





void cProtocol_1_9_0::SendPlayerMaxSpeed(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerMaxSpeed);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteVarInt32(Player->GetUniqueID());
	Pkt.WriteBEInt32(1);  // Count
	Pkt.WriteString("generic.movementSpeed");
	// The default game speed is 0.1, multiply that value by the relative speed:
	Pkt.WriteBEDouble(0.1 * Player->GetNormalMaxSpeed());
	if (Player->IsSprinting())
	{
		Pkt.WriteVarInt32(1);  // Modifier count
		Pkt.WriteBEUInt64(0x662a6b8dda3e4c1c);
		Pkt.WriteBEUInt64(0x881396ea6097278d);  // UUID of the modifier
		Pkt.WriteBEDouble(Player->GetSprintingMaxSpeed() - Player->GetNormalMaxSpeed());
		Pkt.WriteBEUInt8(2);
	}
	else
	{
		Pkt.WriteVarInt32(0);  // Modifier count
	}
}





void cProtocol_1_9_0::SendPlayerMoveLook(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPlayerMoveLook);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteBEDouble(Player->GetPosX());
	Pkt.WriteBEDouble(Player->GetPosY());
	Pkt.WriteBEDouble(Player->GetPosZ());
	Pkt.WriteBEFloat(static_cast<float>(Player->GetYaw()));
	Pkt.WriteBEFloat(static_cast<float>(Player->GetPitch()));
	Pkt.WriteBEUInt8(0);
	Pkt.WriteVarInt32(++m_OutstandingTeleportId);

	// This teleport ID hasn't been confirmed yet
	m_IsTeleportIdConfirmed = false;
}





void cProtocol_1_9_0::SendPlayerPosition(void)
{
	// There is no dedicated packet for this, send the whole thing:
	SendPlayerMoveLook();
}





void cProtocol_1_9_0::SendPlayerSpawn(const cPlayer & a_Player)
{
	// Called to spawn another player for the client
	cPacketizer Pkt(*this, pktSpawnOtherPlayer);
	Pkt.WriteVarInt32(a_Player.GetUniqueID());
	Pkt.WriteUUID(a_Player.GetUUID());
	Vector3d LastSentPos = a_Player.GetLastSentPos();
	Pkt.WriteBEDouble(LastSentPos.x);
	Pkt.WriteBEDouble(LastSentPos.y + 0.001);  // The "+ 0.001" is there because otherwise the player falls through the block they were standing on.
	Pkt.WriteBEDouble(LastSentPos.z);
	Pkt.WriteByteAngle(a_Player.GetYaw());
	Pkt.WriteByteAngle(a_Player.GetPitch());
	WriteEntityMetadata(Pkt, a_Player);
	Pkt.WriteBEUInt8(0xff);  // Metadata: end
}





void cProtocol_1_9_0::SendPluginMessage(const AString & a_Channel, const AString & a_Message)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktPluginMessage);
	Pkt.WriteString(a_Channel);
	Pkt.WriteBuf(a_Message.data(), a_Message.size());
}





void cProtocol_1_9_0::SendRemoveEntityEffect(const cEntity & a_Entity, int a_EffectID)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktRemoveEntityEffect);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_EffectID));
}





void cProtocol_1_9_0::SendResetTitle(void)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTitle);
	Pkt.WriteVarInt32(4);  // Reset title
}





void cProtocol_1_9_0::SendRespawn(eDimension a_Dimension)
{
	cPacketizer Pkt(*this, pktRespawn);
	cPlayer * Player = m_Client->GetPlayer();
	Pkt.WriteBEInt32(static_cast<Int32>(a_Dimension));
	Pkt.WriteBEUInt8(2);  // TODO: Difficulty (set to Normal)
	Pkt.WriteBEUInt8(static_cast<Byte>(Player->GetEffectiveGameMode()));
	Pkt.WriteString("default");
}





void cProtocol_1_9_0::SendScoreboardObjective(const AString & a_Name, const AString & a_DisplayName, Byte a_Mode)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktScoreboardObjective);
	Pkt.WriteString(a_Name);
	Pkt.WriteBEUInt8(a_Mode);
	if ((a_Mode == 0) || (a_Mode == 2))
	{
		Pkt.WriteString(a_DisplayName);
		Pkt.WriteString("integer");
	}
}





void cProtocol_1_9_0::SendScoreUpdate(const AString & a_Objective, const AString & a_Player, cObjective::Score a_Score, Byte a_Mode)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUpdateScore);
	Pkt.WriteString(a_Player);
	Pkt.WriteBEUInt8(a_Mode);
	Pkt.WriteString(a_Objective);

	if (a_Mode != 1)
	{
		Pkt.WriteVarInt32(static_cast<UInt32>(a_Score));
	}
}





void cProtocol_1_9_0::SendDisplayObjective(const AString & a_Objective, cScoreboard::eDisplaySlot a_Display)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktDisplayObjective);
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_Display));
	Pkt.WriteString(a_Objective);
}





void cProtocol_1_9_0::SendSetSubTitle(const cCompositeChat & a_SubTitle)
{
	SendSetRawSubTitle(a_SubTitle.CreateJsonString(false));
}





void cProtocol_1_9_0::SendSetRawSubTitle(const AString & a_SubTitle)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTitle);
	Pkt.WriteVarInt32(1);  // Set subtitle
	Pkt.WriteString(a_SubTitle);
}





void cProtocol_1_9_0::SendSetTitle(const cCompositeChat & a_Title)
{
	SendSetRawTitle(a_Title.CreateJsonString(false));
}





void cProtocol_1_9_0::SendSetRawTitle(const AString & a_Title)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTitle);
	Pkt.WriteVarInt32(0);  // Set title
	Pkt.WriteString(a_Title);
}





void cProtocol_1_9_0::SendSoundEffect(const AString & a_SoundName, double a_X, double a_Y, double a_Z, float a_Volume, float a_Pitch)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSoundEffect);
	Pkt.WriteString(a_SoundName);
	Pkt.WriteVarInt32(0);  // Master sound category (may want to be changed to a parameter later)
	Pkt.WriteBEInt32(static_cast<Int32>(a_X * 8.0));
	Pkt.WriteBEInt32(static_cast<Int32>(a_Y * 8.0));
	Pkt.WriteBEInt32(static_cast<Int32>(a_Z * 8.0));
	Pkt.WriteBEFloat(a_Volume);
	Pkt.WriteBEUInt8(static_cast<Byte>(a_Pitch * 63));
}





void cProtocol_1_9_0::SendSoundParticleEffect(const EffectID a_EffectID, int a_SrcX, int a_SrcY, int a_SrcZ, int a_Data)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSoundParticleEffect);
	Pkt.WriteBEInt32(static_cast<int>(a_EffectID));
	Pkt.WritePosition64(a_SrcX, a_SrcY, a_SrcZ);
	Pkt.WriteBEInt32(a_Data);
	Pkt.WriteBool(false);
}





void cProtocol_1_9_0::SendSpawnFallingBlock(const cFallingBlock & a_FallingBlock)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSpawnObject);
	Pkt.WriteVarInt32(a_FallingBlock.GetUniqueID());
	// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
	Pkt.WriteBEUInt64(0);
	Pkt.WriteBEUInt64(a_FallingBlock.GetUniqueID());
	Pkt.WriteBEUInt8(70);  // Falling block
	Vector3d LastSentPos = a_FallingBlock.GetLastSentPos();
	Pkt.WriteBEDouble(LastSentPos.x);
	Pkt.WriteBEDouble(LastSentPos.y);
	Pkt.WriteBEDouble(LastSentPos.z);
	Pkt.WriteByteAngle(a_FallingBlock.GetYaw());
	Pkt.WriteByteAngle(a_FallingBlock.GetPitch());
	Pkt.WriteBEInt32(static_cast<Int32>(a_FallingBlock.GetBlockType()) | (static_cast<Int32>(a_FallingBlock.GetBlockMeta()) << 12));
	Pkt.WriteBEInt16(static_cast<Int16>(a_FallingBlock.GetSpeedX() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_FallingBlock.GetSpeedY() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_FallingBlock.GetSpeedZ() * 400));
}





void cProtocol_1_9_0::SendSpawnMob(const cMonster & a_Mob)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSpawnMob);
	Pkt.WriteVarInt32(a_Mob.GetUniqueID());
	// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
	Pkt.WriteBEUInt64(0);
	Pkt.WriteBEUInt64(a_Mob.GetUniqueID());
	Pkt.WriteBEUInt8(static_cast<Byte>(a_Mob.GetMobType()));
	Vector3d LastSentPos = a_Mob.GetLastSentPos();
	Pkt.WriteBEDouble(LastSentPos.x);
	Pkt.WriteBEDouble(LastSentPos.y);
	Pkt.WriteBEDouble(LastSentPos.z);
	Pkt.WriteByteAngle(a_Mob.GetPitch());
	Pkt.WriteByteAngle(a_Mob.GetHeadYaw());
	Pkt.WriteByteAngle(a_Mob.GetYaw());
	Pkt.WriteBEInt16(static_cast<Int16>(a_Mob.GetSpeedX() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Mob.GetSpeedY() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Mob.GetSpeedZ() * 400));
	WriteEntityMetadata(Pkt, a_Mob);
	Pkt.WriteBEUInt8(0xff);  // Metadata terminator
}





void cProtocol_1_9_0::SendSpawnObject(const cEntity & a_Entity, char a_ObjectType, int a_ObjectData, Byte a_Yaw, Byte a_Pitch)
{
	ASSERT(m_State == 3);  // In game mode?
	double PosX = a_Entity.GetPosX();
	double PosZ = a_Entity.GetPosZ();
	double Yaw = a_Entity.GetYaw();
	if (a_ObjectType == 71)
	{
		FixItemFramePositions(a_ObjectData, PosX, PosZ, Yaw);
	}

	cPacketizer Pkt(*this, pktSpawnObject);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
	Pkt.WriteBEUInt64(0);
	Pkt.WriteBEUInt64(a_Entity.GetUniqueID());
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_ObjectType));
	Pkt.WriteBEDouble(PosX);
	Pkt.WriteBEDouble(a_Entity.GetPosY());
	Pkt.WriteBEDouble(PosZ);
	Pkt.WriteByteAngle(a_Entity.GetPitch());
	Pkt.WriteByteAngle(Yaw);
	Pkt.WriteBEInt32(a_ObjectData);
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedX() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedY() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Entity.GetSpeedZ() * 400));
}





void cProtocol_1_9_0::SendSpawnVehicle(const cEntity & a_Vehicle, char a_VehicleType, char a_VehicleSubType)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSpawnObject);
	Pkt.WriteVarInt32(a_Vehicle.GetUniqueID());
	// TODO: Bad way to write a UUID, and it's not a true UUID, but this is functional for now.
	Pkt.WriteBEUInt64(0);
	Pkt.WriteBEUInt64(a_Vehicle.GetUniqueID());
	Pkt.WriteBEUInt8(static_cast<UInt8>(a_VehicleType));
	Vector3d LastSentPos = a_Vehicle.GetLastSentPos();
	Pkt.WriteBEDouble(LastSentPos.x);
	Pkt.WriteBEDouble(LastSentPos.y);
	Pkt.WriteBEDouble(LastSentPos.z);
	Pkt.WriteByteAngle(a_Vehicle.GetPitch());
	Pkt.WriteByteAngle(a_Vehicle.GetYaw());
	Pkt.WriteBEInt32(a_VehicleSubType);
	Pkt.WriteBEInt16(static_cast<Int16>(a_Vehicle.GetSpeedX() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Vehicle.GetSpeedY() * 400));
	Pkt.WriteBEInt16(static_cast<Int16>(a_Vehicle.GetSpeedZ() * 400));
}





void cProtocol_1_9_0::SendStatistics(const cStatManager & a_Manager)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktStatistics);
	Pkt.WriteVarInt32(statCount);  // TODO 2014-05-11 xdot: Optimization: Send "dirty" statistics only

	size_t Count = static_cast<size_t>(statCount);
	for (size_t i = 0; i < Count; ++i)
	{
		StatValue Value = a_Manager.GetValue(static_cast<eStatistic>(i));
		const AString & StatName = cStatInfo::GetName(static_cast<eStatistic>(i));

		Pkt.WriteString(StatName);
		Pkt.WriteVarInt32(static_cast<UInt32>(Value));
	}
}





void cProtocol_1_9_0::SendTabCompletionResults(const AStringVector & a_Results)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTabCompletionResults);
	Pkt.WriteVarInt32(static_cast<UInt32>(a_Results.size()));

	for (AStringVector::const_iterator itr = a_Results.begin(), end = a_Results.end(); itr != end; ++itr)
	{
		Pkt.WriteString(*itr);
	}
}





void cProtocol_1_9_0::SendTeleportEntity(const cEntity & a_Entity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTeleportEntity);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WriteBEDouble(a_Entity.GetPosX());
	Pkt.WriteBEDouble(a_Entity.GetPosY());
	Pkt.WriteBEDouble(a_Entity.GetPosZ());
	Pkt.WriteByteAngle(a_Entity.GetYaw());
	Pkt.WriteByteAngle(a_Entity.GetPitch());
	Pkt.WriteBool(a_Entity.IsOnGround());
}





void cProtocol_1_9_0::SendThunderbolt(int a_BlockX, int a_BlockY, int a_BlockZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktSpawnGlobalEntity);
	Pkt.WriteVarInt32(0);  // EntityID = 0, always
	Pkt.WriteBEUInt8(1);  // Type = Thunderbolt
	Pkt.WriteBEDouble(a_BlockX);
	Pkt.WriteBEDouble(a_BlockY);
	Pkt.WriteBEDouble(a_BlockZ);
}





void cProtocol_1_9_0::SendTitleTimes(int a_FadeInTicks, int a_DisplayTicks, int a_FadeOutTicks)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktTitle);
	Pkt.WriteVarInt32(2);  // Set title display times
	Pkt.WriteBEInt32(a_FadeInTicks);
	Pkt.WriteBEInt32(a_DisplayTicks);
	Pkt.WriteBEInt32(a_FadeOutTicks);
}





void cProtocol_1_9_0::SendTimeUpdate(Int64 a_WorldAge, Int64 a_TimeOfDay, bool a_DoDaylightCycle)
{
	ASSERT(m_State == 3);  // In game mode?
	if (!a_DoDaylightCycle)
	{
		// When writing a "-" before the number the client ignores it but it will stop the client-side time expiration.
		a_TimeOfDay = std::min(-a_TimeOfDay, -1LL);
	}

	cPacketizer Pkt(*this, pktTimeUpdate);
	Pkt.WriteBEInt64(a_WorldAge);
	Pkt.WriteBEInt64(a_TimeOfDay);
}





void cProtocol_1_9_0::SendUnloadChunk(int a_ChunkX, int a_ChunkZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUnloadChunk);
	Pkt.WriteBEInt32(a_ChunkX);
	Pkt.WriteBEInt32(a_ChunkZ);
}





void cProtocol_1_9_0::SendUpdateBlockEntity(cBlockEntity & a_BlockEntity)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUpdateBlockEntity);
	Pkt.WritePosition64(a_BlockEntity.GetPosX(), a_BlockEntity.GetPosY(), a_BlockEntity.GetPosZ());
	Byte Action = 0;
	switch (a_BlockEntity.GetBlockType())
	{
		case E_BLOCK_MOB_SPAWNER:   Action = 1; break;  // Update mob spawner spinny mob thing
		case E_BLOCK_COMMAND_BLOCK: Action = 2; break;  // Update command block text
		case E_BLOCK_BEACON:        Action = 3; break;  // Update beacon entity
		case E_BLOCK_HEAD:          Action = 4; break;  // Update Mobhead entity
		case E_BLOCK_FLOWER_POT:    Action = 5; break;  // Update flower pot
		case E_BLOCK_BED:           Action = 11; break;  // Update bed color
		default: ASSERT(!"Unhandled or unimplemented BlockEntity update request!"); break;
	}
	Pkt.WriteBEUInt8(Action);

	WriteBlockEntity(Pkt, a_BlockEntity);
}





void cProtocol_1_9_0::SendUpdateSign(int a_BlockX, int a_BlockY, int a_BlockZ, const AString & a_Line1, const AString & a_Line2, const AString & a_Line3, const AString & a_Line4)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUpdateSign);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);

	Json::StyledWriter JsonWriter;
	AString Lines[] = { a_Line1, a_Line2, a_Line3, a_Line4 };
	for (size_t i = 0; i < ARRAYCOUNT(Lines); i++)
	{
		Json::Value RootValue;
		RootValue["text"] = Lines[i];
		Pkt.WriteString(JsonWriter.write(RootValue).c_str());
	}
}





void cProtocol_1_9_0::SendUseBed(const cEntity & a_Entity, int a_BlockX, int a_BlockY, int a_BlockZ)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktUseBed);
	Pkt.WriteVarInt32(a_Entity.GetUniqueID());
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
}





void cProtocol_1_9_0::SendWeather(eWeather a_Weather)
{
	ASSERT(m_State == 3);  // In game mode?

	{
		cPacketizer Pkt(*this, pktWeather);
		Pkt.WriteBEUInt8((a_Weather == wSunny) ? 1 : 2);  // End rain / begin rain
		Pkt.WriteBEFloat(0);  // Unused for weather
	}

	// TODO: Fade effect, somehow
}





void cProtocol_1_9_0::SendWholeInventory(const cWindow & a_Window)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktWindowItems);
	Pkt.WriteBEInt8(a_Window.GetWindowID());
	Pkt.WriteBEInt16(static_cast<Int16>(a_Window.GetNumSlots()));
	cItems Slots;
	a_Window.GetSlots(*(m_Client->GetPlayer()), Slots);
	for (cItems::const_iterator itr = Slots.begin(), end = Slots.end(); itr != end; ++itr)
	{
		WriteItem(Pkt, *itr);
	}  // for itr - Slots[]
}





void cProtocol_1_9_0::SendWindowClose(const cWindow & a_Window)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktWindowClose);
	Pkt.WriteBEInt8(a_Window.GetWindowID());
}





void cProtocol_1_9_0::SendWindowOpen(const cWindow & a_Window)
{
	ASSERT(m_State == 3);  // In game mode?

	if (a_Window.GetWindowType() < 0)
	{
		// Do not send this packet for player inventory windows
		return;
	}

	cPacketizer Pkt(*this, pktWindowOpen);
	Pkt.WriteBEInt8(a_Window.GetWindowID());
	Pkt.WriteString(a_Window.GetWindowTypeName());
	Pkt.WriteString(Printf("{\"text\":\"%s\"}", a_Window.GetWindowTitle().c_str()));

	switch (a_Window.GetWindowType())
	{
		case cWindow::wtWorkbench:
		case cWindow::wtEnchantment:
		case cWindow::wtAnvil:
		{
			Pkt.WriteBEInt8(0);
			break;
		}
		default:
		{
			Pkt.WriteBEInt8(static_cast<Int8>(a_Window.GetNumNonInventorySlots()));
			break;
		}
	}

	if (a_Window.GetWindowType() == cWindow::wtAnimalChest)
	{
		UInt32 HorseID = static_cast<const cHorseWindow &>(a_Window).GetHorseID();
		Pkt.WriteBEInt32(static_cast<Int32>(HorseID));
	}
}





void cProtocol_1_9_0::SendWindowProperty(const cWindow & a_Window, short a_Property, short a_Value)
{
	ASSERT(m_State == 3);  // In game mode?

	cPacketizer Pkt(*this, pktWindowProperty);
	Pkt.WriteBEInt8(a_Window.GetWindowID());
	Pkt.WriteBEInt16(a_Property);
	Pkt.WriteBEInt16(a_Value);
}





bool cProtocol_1_9_0::CompressPacket(const AString & a_Packet, AString & a_CompressedData)
{
	// Compress the data:
	char CompressedData[MAX_COMPRESSED_PACKET_LEN];

	uLongf CompressedSize = compressBound(static_cast<uLongf>(a_Packet.size()));
	if (CompressedSize >= MAX_COMPRESSED_PACKET_LEN)
	{
		ASSERT(!"Too high packet size.");
		return false;
	}

	int Status = compress2(
		reinterpret_cast<Bytef *>(CompressedData), &CompressedSize,
		reinterpret_cast<const Bytef *>(a_Packet.data()), static_cast<uLongf>(a_Packet.size()), Z_DEFAULT_COMPRESSION
	);
	if (Status != Z_OK)
	{
		return false;
	}

	AString LengthData;
	cByteBuffer Buffer(20);
	Buffer.WriteVarInt32(static_cast<UInt32>(a_Packet.size()));
	Buffer.ReadAll(LengthData);
	Buffer.CommitRead();

	Buffer.WriteVarInt32(static_cast<UInt32>(CompressedSize + LengthData.size()));
	Buffer.WriteVarInt32(static_cast<UInt32>(a_Packet.size()));
	Buffer.ReadAll(LengthData);
	Buffer.CommitRead();

	a_CompressedData.clear();
	a_CompressedData.reserve(LengthData.size() + CompressedSize);
	a_CompressedData.append(LengthData.data(), LengthData.size());
	a_CompressedData.append(CompressedData, CompressedSize);
	return true;
}





int cProtocol_1_9_0::GetParticleID(const AString & a_ParticleName)
{
	static std::map<AString, int> ParticleMap;
	if (ParticleMap.empty())
	{
		// Initialize the ParticleMap:
		ParticleMap["explode"]          = 0;
		ParticleMap["largeexplode"]     = 1;
		ParticleMap["hugeexplosion"]    = 2;
		ParticleMap["fireworksspark"]   = 3;
		ParticleMap["bubble"]           = 4;
		ParticleMap["splash"]           = 5;
		ParticleMap["wake"]             = 6;
		ParticleMap["suspended"]        = 7;
		ParticleMap["depthsuspend"]     = 8;
		ParticleMap["crit"]             = 9;
		ParticleMap["magiccrit"]        = 10;
		ParticleMap["smoke"]            = 11;
		ParticleMap["largesmoke"]       = 12;
		ParticleMap["spell"]            = 13;
		ParticleMap["instantspell"]     = 14;
		ParticleMap["mobspell"]         = 15;
		ParticleMap["mobspellambient"]  = 16;
		ParticleMap["witchmagic"]       = 17;
		ParticleMap["dripwater"]        = 18;
		ParticleMap["driplava"]         = 19;
		ParticleMap["angryvillager"]    = 20;
		ParticleMap["happyvillager"]    = 21;
		ParticleMap["townaura"]         = 22;
		ParticleMap["note"]             = 23;
		ParticleMap["portal"]           = 24;
		ParticleMap["enchantmenttable"] = 25;
		ParticleMap["flame"]            = 26;
		ParticleMap["lava"]             = 27;
		ParticleMap["footstep"]         = 28;
		ParticleMap["cloud"]            = 29;
		ParticleMap["reddust"]          = 30;
		ParticleMap["snowballpoof"]     = 31;
		ParticleMap["snowshovel"]       = 32;
		ParticleMap["slime"]            = 33;
		ParticleMap["heart"]            = 34;
		ParticleMap["barrier"]          = 35;
		ParticleMap["iconcrack"]        = 36;
		ParticleMap["blockcrack"]       = 37;
		ParticleMap["blockdust"]        = 38;
		ParticleMap["droplet"]          = 39;
		ParticleMap["take"]             = 40;
		ParticleMap["mobappearance"]    = 41;
		ParticleMap["dragonbreath"]     = 42;
		ParticleMap["endrod"]           = 43;
		ParticleMap["damageindicator"]  = 44;
		ParticleMap["sweepattack"]      = 45;
		ParticleMap["fallingdust"]      = 46;
		ParticleMap["totem"]            = 47;
		ParticleMap["spit"]             = 48;
	}

	AString ParticleName = StrToLower(a_ParticleName);
	if (ParticleMap.find(ParticleName) == ParticleMap.end())
	{
		LOGWARNING("Unknown particle: %s", a_ParticleName.c_str());
		ASSERT(!"Unknown particle");
		return 0;
	}

	return ParticleMap[ParticleName];
}





void cProtocol_1_9_0::FixItemFramePositions(int a_ObjectData, double & a_PosX, double & a_PosZ, double & a_Yaw)
{
	switch (a_ObjectData)
	{
		case 0:
		{
			a_PosZ += 1;
			a_Yaw = 0;
			break;
		}
		case 1:
		{
			a_PosX -= 1;
			a_Yaw = 90;
			break;
		}
		case 2:
		{
			a_PosZ -= 1;
			a_Yaw = 180;
			break;
		}
		case 3:
		{
			a_PosX += 1;
			a_Yaw = 270;
			break;
		}
	}
}





void cProtocol_1_9_0::AddReceivedData(const char * a_Data, size_t a_Size)
{
	// Write the incoming data into the comm log file:
	if (g_ShouldLogCommIn && m_CommLogFile.IsOpen())
	{
		if (m_ReceivedData.GetReadableSpace() > 0)
		{
			AString AllData;
			size_t OldReadableSpace = m_ReceivedData.GetReadableSpace();
			m_ReceivedData.ReadAll(AllData);
			m_ReceivedData.ResetRead();
			m_ReceivedData.SkipRead(m_ReceivedData.GetReadableSpace() - OldReadableSpace);
			ASSERT(m_ReceivedData.GetReadableSpace() == OldReadableSpace);
			AString Hex;
			CreateHexDump(Hex, AllData.data(), AllData.size(), 16);
			m_CommLogFile.Printf("Incoming data, %zu (0x%zx) unparsed bytes already present in buffer:\n%s\n",
				AllData.size(), AllData.size(), Hex.c_str()
			);
		}
		AString Hex;
		CreateHexDump(Hex, a_Data, a_Size, 16);
		m_CommLogFile.Printf("Incoming data: %u (0x%x) bytes: \n%s\n",
			static_cast<unsigned>(a_Size), static_cast<unsigned>(a_Size), Hex.c_str()
		);
		m_CommLogFile.Flush();
	}

	if (!m_ReceivedData.Write(a_Data, a_Size))
	{
		// Too much data in the incoming queue, report to caller:
		m_Client->PacketBufferFull();
		return;
	}

	// Handle all complete packets:
	for (;;)
	{
		UInt32 PacketLen;
		if (!m_ReceivedData.ReadVarInt(PacketLen))
		{
			// Not enough data
			m_ReceivedData.ResetRead();
			break;
		}
		if (!m_ReceivedData.CanReadBytes(PacketLen))
		{
			// The full packet hasn't been received yet
			m_ReceivedData.ResetRead();
			break;
		}

		// Check packet for compression:
		UInt32 UncompressedSize = 0;
		AString UncompressedData;
		if (m_State == 3)
		{
			UInt32 NumBytesRead = static_cast<UInt32>(m_ReceivedData.GetReadableSpace());

			if (!m_ReceivedData.ReadVarInt(UncompressedSize))
			{
				m_Client->Kick("Compression packet incomplete");
				return;
			}

			NumBytesRead -= static_cast<UInt32>(m_ReceivedData.GetReadableSpace());  // How many bytes has the UncompressedSize taken up?
			ASSERT(PacketLen > NumBytesRead);
			PacketLen -= NumBytesRead;

			if (UncompressedSize > 0)
			{
				// Decompress the data:
				AString CompressedData;
				VERIFY(m_ReceivedData.ReadString(CompressedData, PacketLen));
				if (InflateString(CompressedData.data(), PacketLen, UncompressedData) != Z_OK)
				{
					m_Client->Kick("Compression failure");
					return;
				}
				PacketLen = static_cast<UInt32>(UncompressedData.size());
				if (PacketLen != UncompressedSize)
				{
					m_Client->Kick("Wrong uncompressed packet size given");
					return;
				}
			}
		}

		// Move the packet payload to a separate cByteBuffer, bb:
		cByteBuffer bb(PacketLen + 1);
		if (UncompressedSize == 0)
		{
			// No compression was used, move directly
			VERIFY(m_ReceivedData.ReadToByteBuffer(bb, static_cast<size_t>(PacketLen)));
		}
		else
		{
			// Compression was used, move the uncompressed data:
			VERIFY(bb.Write(UncompressedData.data(), UncompressedData.size()));
		}
		m_ReceivedData.CommitRead();

		UInt32 PacketType;
		if (!bb.ReadVarInt(PacketType))
		{
			// Not enough data
			break;
		}

		// Write one NUL extra, so that we can detect over-reads
		bb.Write("\0", 1);

		// Log the packet info into the comm log file:
		if (g_ShouldLogCommIn && m_CommLogFile.IsOpen())
		{
			AString PacketData;
			bb.ReadAll(PacketData);
			bb.ResetRead();
			bb.ReadVarInt(PacketType);  // We have already read the packet type once, it will be there again
			ASSERT(PacketData.size() > 0);  // We have written an extra NUL, so there had to be at least one byte read
			PacketData.resize(PacketData.size() - 1);
			AString PacketDataHex;
			CreateHexDump(PacketDataHex, PacketData.data(), PacketData.size(), 16);
			m_CommLogFile.Printf("Next incoming packet is type %u (0x%x), length %u (0x%x) at state %d. Payload:\n%s\n",
				PacketType, PacketType, PacketLen, PacketLen, m_State, PacketDataHex.c_str()
			);
		}

		if (!HandlePacket(bb, PacketType))
		{
			// Unknown packet, already been reported, but without the length. Log the length here:
			LOGWARNING("Protocol 1.9: Unhandled packet: type 0x%x, state %d, length %u", PacketType, m_State, PacketLen);

			#ifdef _DEBUG
				// Dump the packet contents into the log:
				bb.ResetRead();
				AString Packet;
				bb.ReadAll(Packet);
				Packet.resize(Packet.size() - 1);  // Drop the final NUL pushed there for over-read detection
				AString Out;
				CreateHexDump(Out, Packet.data(), Packet.size(), 24);
				LOGD("Packet contents:\n%s", Out.c_str());
			#endif  // _DEBUG

			// Put a message in the comm log:
			if (g_ShouldLogCommIn && m_CommLogFile.IsOpen())
			{
				m_CommLogFile.Printf("^^^^^^ Unhandled packet ^^^^^^\n\n\n");
			}

			return;
		}

		// The packet should have 1 byte left in the buffer - the NUL we had added
		if (bb.GetReadableSpace() != 1)
		{
			// Read more or less than packet length, report as error
			LOGWARNING("Protocol 1.9: Wrong number of bytes read for packet 0x%x, state %d. Read %zu bytes, packet contained %u bytes",
				PacketType, m_State, bb.GetUsedSpace() - bb.GetReadableSpace(), PacketLen
			);

			// Put a message in the comm log:
			if (g_ShouldLogCommIn && m_CommLogFile.IsOpen())
			{
				m_CommLogFile.Printf("^^^^^^ Wrong number of bytes read for this packet (exp %d left, got %zu left) ^^^^^^\n\n\n",
					1, bb.GetReadableSpace()
				);
				m_CommLogFile.Flush();
			}

			ASSERT(!"Read wrong number of bytes!");
			m_Client->PacketError(PacketType);
		}
	}  // for (ever)

	// Log any leftover bytes into the logfile:
	if (g_ShouldLogCommIn && (m_ReceivedData.GetReadableSpace() > 0) && m_CommLogFile.IsOpen())
	{
		AString AllData;
		size_t OldReadableSpace = m_ReceivedData.GetReadableSpace();
		m_ReceivedData.ReadAll(AllData);
		m_ReceivedData.ResetRead();
		m_ReceivedData.SkipRead(m_ReceivedData.GetReadableSpace() - OldReadableSpace);
		ASSERT(m_ReceivedData.GetReadableSpace() == OldReadableSpace);
		AString Hex;
		CreateHexDump(Hex, AllData.data(), AllData.size(), 16);
		m_CommLogFile.Printf("Protocol 1.9: There are %zu (0x%zx) bytes of non-parse-able data left in the buffer:\n%s",
			m_ReceivedData.GetReadableSpace(), m_ReceivedData.GetReadableSpace(), Hex.c_str()
		);
		m_CommLogFile.Flush();
	}
}





UInt32 cProtocol_1_9_0::GetPacketID(cProtocol::ePacketType a_Packet)
{
	switch (a_Packet)
	{
		case pktAttachEntity:          return 0x40;
		case pktBlockAction:           return 0x0a;
		case pktBlockBreakAnim:        return 0x08;
		case pktBlockChange:           return 0x0b;
		case pktBlockChanges:          return 0x10;
		case pktCameraSetTo:           return 0x36;
		case pktChatRaw:               return 0x0f;
		case pktCollectEntity:         return 0x49;
		case pktDestroyEntity:         return 0x30;
		case pktDifficulty:            return 0x0d;
		case pktDisconnectDuringGame:  return 0x1a;
		case pktDisconnectDuringLogin: return 0x0;
		case pktDisplayObjective:      return 0x38;
		case pktEditSign:              return 0x2a;
		case pktEncryptionRequest:     return 0x01;
		case pktEntityAnimation:       return 0x06;
		case pktEntityEffect:          return 0x4c;
		case pktEntityEquipment:       return 0x3c;
		case pktEntityHeadLook:        return 0x34;
		case pktEntityLook:            return 0x27;
		case pktEntityMeta:            return 0x39;
		case pktEntityProperties:      return 0x4b;
		case pktEntityRelMove:         return 0x25;
		case pktEntityRelMoveLook:     return 0x26;
		case pktEntityStatus:          return 0x1b;
		case pktEntityVelocity:        return 0x3b;
		case pktExperience:            return 0x3d;
		case pktExplosion:             return 0x1c;
		case pktGameMode:              return 0x1e;
		case pktHeldItemChange:        return 0x37;
		case pktInventorySlot:         return 0x16;
		case pktJoinGame:              return 0x23;
		case pktKeepAlive:             return 0x1f;
		case pktLeashEntity:           return 0x3a;
		case pktLoginSuccess:          return 0x02;
		case pktMapData:               return 0x24;
		case pktParticleEffect:        return 0x22;
		case pktPingResponse:          return 0x01;
		case pktPlayerAbilities:       return 0x2b;
		case pktPlayerList:            return 0x2d;
		case pktPlayerMaxSpeed:        return 0x4b;
		case pktPlayerMoveLook:        return 0x2e;
		case pktPluginMessage:         return 0x18;
		case pktRemoveEntityEffect:    return 0x31;
		case pktRespawn:               return 0x33;
		case pktScoreboardObjective:   return 0x3f;
		case pktSpawnExperienceOrb:    return 0x01;
		case pktSpawnGlobalEntity:     return 0x02;
		case pktSpawnObject:           return 0x00;
		case pktSpawnOtherPlayer:      return 0x05;
		case pktSpawnPainting:         return 0x04;
		case pktSpawnPosition:         return 0x43;
		case pktSoundEffect:           return 0x19;
		case pktSoundParticleEffect:   return 0x21;
		case pktSpawnMob:              return 0x03;
		case pktStartCompression:      return 0x03;
		case pktStatistics:            return 0x07;
		case pktStatusResponse:        return 0x00;
		case pktTabCompletionResults:  return 0x0e;
		case pktTeleportEntity:        return 0x4a;
		case pktTimeUpdate:            return 0x44;
		case pktTitle:                 return 0x45;
		case pktUnloadChunk:           return 0x1d;
		case pktUpdateBlockEntity:     return 0x09;
		case pktUpdateHealth:          return 0x3e;
		case pktUpdateScore:           return 0x42;
		case pktUpdateSign:            return 0x46;
		case pktUseBed:                return 0x2f;
		case pktWeather:               return 0x1e;
		case pktWindowClose:           return 0x12;
		case pktWindowItems:           return 0x14;
		case pktWindowOpen:            return 0x13;
		case pktWindowProperty:        return 0x15;
	}
	UNREACHABLE("Unsupported outgoing packet type");
}





bool cProtocol_1_9_0::HandlePacket(cByteBuffer & a_ByteBuffer, UInt32 a_PacketType)
{
	switch (m_State)
	{
		case 1:
		{
			// Status
			switch (a_PacketType)
			{
				case 0x00: HandlePacketStatusRequest(a_ByteBuffer); return true;
				case 0x01: HandlePacketStatusPing   (a_ByteBuffer); return true;
			}
			break;
		}

		case 2:
		{
			// Login
			switch (a_PacketType)
			{
				case 0x00: HandlePacketLoginStart             (a_ByteBuffer); return true;
				case 0x01: HandlePacketLoginEncryptionResponse(a_ByteBuffer); return true;
			}
			break;
		}

		case 3:
		{
			// Game
			switch (a_PacketType)
			{
				case 0x00: HandleConfirmTeleport              (a_ByteBuffer); return true;
				case 0x01: HandlePacketTabComplete            (a_ByteBuffer); return true;
				case 0x02: HandlePacketChatMessage            (a_ByteBuffer); return true;
				case 0x03: HandlePacketClientStatus           (a_ByteBuffer); return true;
				case 0x04: HandlePacketClientSettings         (a_ByteBuffer); return true;
				case 0x05: break;  // Confirm transaction - not used in MCS
				case 0x06: HandlePacketEnchantItem            (a_ByteBuffer); return true;
				case 0x07: HandlePacketWindowClick            (a_ByteBuffer); return true;
				case 0x08: HandlePacketWindowClose            (a_ByteBuffer); return true;
				case 0x09: HandlePacketPluginMessage          (a_ByteBuffer); return true;
				case 0x0a: HandlePacketUseEntity              (a_ByteBuffer); return true;
				case 0x0b: HandlePacketKeepAlive              (a_ByteBuffer); return true;
				case 0x0c: HandlePacketPlayerPos              (a_ByteBuffer); return true;
				case 0x0d: HandlePacketPlayerPosLook          (a_ByteBuffer); return true;
				case 0x0e: HandlePacketPlayerLook             (a_ByteBuffer); return true;
				case 0x0f: HandlePacketPlayer                 (a_ByteBuffer); return true;
				case 0x10: HandlePacketVehicleMove            (a_ByteBuffer); return true;
				case 0x11: HandlePacketBoatSteer              (a_ByteBuffer); return true;
				case 0x12: HandlePacketPlayerAbilities        (a_ByteBuffer); return true;
				case 0x13: HandlePacketBlockDig               (a_ByteBuffer); return true;
				case 0x14: HandlePacketEntityAction           (a_ByteBuffer); return true;
				case 0x15: HandlePacketSteerVehicle           (a_ByteBuffer); return true;
				case 0x16: break;  // Resource pack status - not yet implemented
				case 0x17: HandlePacketSlotSelect             (a_ByteBuffer); return true;
				case 0x18: HandlePacketCreativeInventoryAction(a_ByteBuffer); return true;
				case 0x19: HandlePacketUpdateSign             (a_ByteBuffer); return true;
				case 0x1a: HandlePacketAnimation              (a_ByteBuffer); return true;
				case 0x1b: HandlePacketSpectate               (a_ByteBuffer); return true;
				case 0x1c: HandlePacketBlockPlace             (a_ByteBuffer); return true;
				case 0x1d: HandlePacketUseItem                (a_ByteBuffer); return true;
			}
			break;
		}
		default:
		{
			// Received a packet in an unknown state, report:
			LOGWARNING("Received a packet in an unknown protocol state %d. Ignoring further packets.", m_State);

			// Cannot kick the client - we don't know this state and thus the packet number for the kick packet

			// Switch to a state when all further packets are silently ignored:
			m_State = 255;
			return false;
		}
		case 255:
		{
			// This is the state used for "not processing packets anymore" when we receive a bad packet from a client.
			// Do not output anything (the caller will do that for us), just return failure
			return false;
		}
	}  // switch (m_State)

	// Unknown packet type, report to the ClientHandle:
	m_Client->PacketUnknown(a_PacketType);
	return false;
}





void cProtocol_1_9_0::HandlePacketStatusPing(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEInt64, Int64, Timestamp);

	cPacketizer Pkt(*this, pktPingResponse);
	Pkt.WriteBEInt64(Timestamp);
}





void cProtocol_1_9_0::HandlePacketStatusRequest(cByteBuffer & a_ByteBuffer)
{
	cServer * Server = cRoot::Get()->GetServer();
	AString ServerDescription = Server->GetDescription();
	auto NumPlayers = static_cast<signed>(Server->GetNumPlayers());
	auto MaxPlayers = static_cast<signed>(Server->GetMaxPlayers());
	AString Favicon = Server->GetFaviconData();
	cRoot::Get()->GetPluginManager()->CallHookServerPing(*m_Client, ServerDescription, NumPlayers, MaxPlayers, Favicon);

	// Version:
	Json::Value Version;
	Version["name"] = "Cuberite 1.9";
	Version["protocol"] = 107;

	// Players:
	Json::Value Players;
	Players["online"] = NumPlayers;
	Players["max"] = MaxPlayers;
	// TODO: Add "sample"

	// Description:
	Json::Value Description;
	Description["text"] = ServerDescription.c_str();

	// Create the response:
	Json::Value ResponseValue;
	ResponseValue["version"] = Version;
	ResponseValue["players"] = Players;
	ResponseValue["description"] = Description;
	m_Client->ForgeAugmentServerListPing(ResponseValue);
	if (!Favicon.empty())
	{
		ResponseValue["favicon"] = Printf("data:image/png;base64,%s", Favicon.c_str());
	}

	Json::FastWriter Writer;
	AString Response = Writer.write(ResponseValue);

	cPacketizer Pkt(*this, pktStatusResponse);
	Pkt.WriteString(Response);
}





void cProtocol_1_9_0::HandlePacketLoginEncryptionResponse(cByteBuffer & a_ByteBuffer)
{
	UInt32 EncKeyLength, EncNonceLength;
	if (!a_ByteBuffer.ReadVarInt(EncKeyLength))
	{
		return;
	}
	AString EncKey;
	if (!a_ByteBuffer.ReadString(EncKey, EncKeyLength))
	{
		return;
	}
	if (!a_ByteBuffer.ReadVarInt(EncNonceLength))
	{
		return;
	}
	AString EncNonce;
	if (!a_ByteBuffer.ReadString(EncNonce, EncNonceLength))
	{
		return;
	}
	if ((EncKeyLength > MAX_ENC_LEN) || (EncNonceLength > MAX_ENC_LEN))
	{
		LOGD("Too long encryption");
		m_Client->Kick("Hacked client");
		return;
	}

	// Decrypt EncNonce using privkey
	cRsaPrivateKey & rsaDecryptor = cRoot::Get()->GetServer()->GetPrivateKey();
	UInt32 DecryptedNonce[MAX_ENC_LEN / sizeof(Int32)];
	int res = rsaDecryptor.Decrypt(reinterpret_cast<const Byte *>(EncNonce.data()), EncNonce.size(), reinterpret_cast<Byte *>(DecryptedNonce), sizeof(DecryptedNonce));
	if (res != 4)
	{
		LOGD("Bad nonce length: got %d, exp %d", res, 4);
		m_Client->Kick("Hacked client");
		return;
	}
	if (ntohl(DecryptedNonce[0]) != static_cast<unsigned>(reinterpret_cast<uintptr_t>(this)))
	{
		LOGD("Bad nonce value");
		m_Client->Kick("Hacked client");
		return;
	}

	// Decrypt the symmetric encryption key using privkey:
	Byte DecryptedKey[MAX_ENC_LEN];
	res = rsaDecryptor.Decrypt(reinterpret_cast<const Byte *>(EncKey.data()), EncKey.size(), DecryptedKey, sizeof(DecryptedKey));
	if (res != 16)
	{
		LOGD("Bad key length");
		m_Client->Kick("Hacked client");
		return;
	}

	StartEncryption(DecryptedKey);
	m_Client->HandleLogin(m_Client->GetUsername());
}





void cProtocol_1_9_0::HandlePacketLoginStart(cByteBuffer & a_ByteBuffer)
{
	AString Username;
	if (!a_ByteBuffer.ReadVarUTF8String(Username))
	{
		m_Client->Kick("Bad username");
		return;
	}

	if (!m_Client->HandleHandshake(Username))
	{
		// The client is not welcome here, they have been sent a Kick packet already
		return;
	}

	cServer * Server = cRoot::Get()->GetServer();
	// If auth is required, then send the encryption request:
	if (Server->ShouldAuthenticate())
	{
		cPacketizer Pkt(*this, pktEncryptionRequest);
		Pkt.WriteString(Server->GetServerID());
		const AString & PubKeyDer = Server->GetPublicKeyDER();
		Pkt.WriteVarInt32(static_cast<UInt32>(PubKeyDer.size()));
		Pkt.WriteBuf(PubKeyDer.data(), PubKeyDer.size());
		Pkt.WriteVarInt32(4);
		Pkt.WriteBEInt32(static_cast<int>(reinterpret_cast<intptr_t>(this)));  // Using 'this' as the cryptographic nonce, so that we don't have to generate one each time :)
		m_Client->SetUsername(Username);
		return;
	}

	m_Client->HandleLogin(Username);
}





void cProtocol_1_9_0::HandlePacketAnimation(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt, Int32, Hand);

	m_Client->HandleAnimation(0);  // Packet exists solely for arm-swing notification
}





void cProtocol_1_9_0::HandlePacketBlockDig(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, Status);

	int BlockX, BlockY, BlockZ;
	if (!a_ByteBuffer.ReadPosition64(BlockX, BlockY, BlockZ))
	{
		return;
	}

	HANDLE_READ(a_ByteBuffer, ReadVarInt, Int32, Face);
	m_Client->HandleLeftClick(BlockX, BlockY, BlockZ, FaceIntToBlockFace(Face), Status);
}





void cProtocol_1_9_0::HandlePacketBlockPlace(cByteBuffer & a_ByteBuffer)
{
	int BlockX, BlockY, BlockZ;
	if (!a_ByteBuffer.ReadPosition64(BlockX, BlockY, BlockZ))
	{
		return;
	}

	HANDLE_READ(a_ByteBuffer, ReadVarInt, Int32, Face);
	HANDLE_READ(a_ByteBuffer, ReadVarInt, Int32, Hand);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, CursorX);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, CursorY);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, CursorZ);
	m_Client->HandleRightClick(BlockX, BlockY, BlockZ, FaceIntToBlockFace(Face), CursorX, CursorY, CursorZ, HandIntToEnum(Hand));
}





void cProtocol_1_9_0::HandlePacketBoatSteer(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBool, bool, RightPaddle);
	HANDLE_READ(a_ByteBuffer, ReadBool, bool, LeftPaddle);

	// Get the players vehicle
	cPlayer * Player = m_Client->GetPlayer();
	cEntity * Vehicle = Player->GetAttached();

	if (Vehicle)
	{
		if (Vehicle->GetEntityType() == cEntity::etBoat)
		{
			auto * Boat = static_cast<cBoat *>(Vehicle);
			Boat->UpdatePaddles(RightPaddle, LeftPaddle);
		}
	}
}





void cProtocol_1_9_0::HandlePacketChatMessage(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Message);
	m_Client->HandleChat(Message);
}





void cProtocol_1_9_0::HandlePacketClientSettings(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Locale);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8,       UInt8,   ViewDistance);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8,       UInt8,   ChatFlags);
	HANDLE_READ(a_ByteBuffer, ReadBool,          bool,    ChatColors);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8,       UInt8,   SkinParts);
	HANDLE_READ(a_ByteBuffer, ReadVarInt,        UInt32,   MainHand);

	m_Client->SetLocale(Locale);
	m_Client->SetViewDistance(ViewDistance);
	m_Client->GetPlayer()->SetSkinParts(SkinParts);
	m_Client->GetPlayer()->SetMainHand(static_cast<eMainHand>(MainHand));
	// TODO: Handle chat flags and chat colors
}





void cProtocol_1_9_0::HandlePacketClientStatus(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, ActionID);
	switch (ActionID)
	{
		case 0:
		{
			// Respawn
			m_Client->HandleRespawn();
			break;
		}
		case 1:
		{
			// Request stats
			const cStatManager & Manager = m_Client->GetPlayer()->GetStatManager();
			SendStatistics(Manager);

			break;
		}
		case 2:
		{
			// Open Inventory achievement
			m_Client->GetPlayer()->AwardAchievement(achOpenInv);
			break;
		}
	}
}





void cProtocol_1_9_0::HandleConfirmTeleport(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt32, UInt32, TeleportID);

	// Can we stop throwing away incoming player position packets?
	if (TeleportID == m_OutstandingTeleportId)
	{
		m_IsTeleportIdConfirmed = true;
	}
}





void cProtocol_1_9_0::HandlePacketCreativeInventoryAction(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEInt16, Int16, SlotNum);
	cItem Item;
	if (!ReadItem(a_ByteBuffer, Item))
	{
		return;
	}
	m_Client->HandleCreativeInventory(SlotNum, Item, (SlotNum == -1) ? caLeftClickOutside : caLeftClick);
}





void cProtocol_1_9_0::HandlePacketEntityAction(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt,  UInt32, PlayerID);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8,  Action);
	HANDLE_READ(a_ByteBuffer, ReadVarInt,  UInt32, JumpBoost);

	switch (Action)
	{
		case 0: m_Client->HandleEntityCrouch(PlayerID, true);     break;  // Crouch
		case 1: m_Client->HandleEntityCrouch(PlayerID, false);    break;  // Uncrouch
		case 2: m_Client->HandleEntityLeaveBed(PlayerID);         break;  // Leave Bed
		case 3: m_Client->HandleEntitySprinting(PlayerID, true);  break;  // Start sprinting
		case 4: m_Client->HandleEntitySprinting(PlayerID, false); break;  // Stop sprinting
		case 7: m_Client->HandleOpenHorseInventory(PlayerID);     break;  // Open horse inventory
	}
}





void cProtocol_1_9_0::HandlePacketKeepAlive(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt, UInt32, KeepAliveID);
	m_Client->HandleKeepAlive(KeepAliveID);
}





void cProtocol_1_9_0::HandlePacketPlayer(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBool, bool, IsOnGround);
	// TODO: m_Client->HandlePlayerOnGround(IsOnGround);
}





void cProtocol_1_9_0::HandlePacketPlayerAbilities(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, Flags);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, FlyingSpeed);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, WalkingSpeed);

	// COnvert the bitfield into individual boolean flags:
	bool IsFlying = false, CanFly = false;
	if ((Flags & 2) != 0)
	{
		IsFlying = true;
	}
	if ((Flags & 4) != 0)
	{
		CanFly = true;
	}

	m_Client->HandlePlayerAbilities(CanFly, IsFlying, FlyingSpeed, WalkingSpeed);
}





void cProtocol_1_9_0::HandlePacketPlayerLook(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, Yaw);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, Pitch);
	HANDLE_READ(a_ByteBuffer, ReadBool,    bool,  IsOnGround);
	m_Client->HandlePlayerLook(Yaw, Pitch, IsOnGround);
}





void cProtocol_1_9_0::HandlePacketPlayerPos(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosX);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosY);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosZ);
	HANDLE_READ(a_ByteBuffer, ReadBool,     bool,   IsOnGround);

	if (m_IsTeleportIdConfirmed)
	{
		m_Client->HandlePlayerPos(PosX, PosY, PosZ, PosY + (m_Client->GetPlayer()->IsCrouched() ? 1.54 : 1.62), IsOnGround);
	}
}





void cProtocol_1_9_0::HandlePacketPlayerPosLook(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosX);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosY);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, PosZ);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat,  float,  Yaw);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat,  float,  Pitch);
	HANDLE_READ(a_ByteBuffer, ReadBool,     bool,   IsOnGround);

	if (m_IsTeleportIdConfirmed)
	{
		m_Client->HandlePlayerMoveLook(PosX, PosY, PosZ, PosY + 1.62, Yaw, Pitch, IsOnGround);
	}
}





void cProtocol_1_9_0::HandlePacketPluginMessage(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Channel);

	// If the plugin channel is recognized vanilla, handle it directly:
	if (Channel.substr(0, 3) == "MC|")
	{
		HandleVanillaPluginMessage(a_ByteBuffer, Channel);

		// Skip any unread data (vanilla sometimes sends garbage at the end of a packet; #1692):
		if (a_ByteBuffer.GetReadableSpace() > 1)
		{
			LOGD("Protocol 1.8: Skipping garbage data at the end of a vanilla PluginMessage packet, %u bytes",
				static_cast<unsigned>(a_ByteBuffer.GetReadableSpace() - 1)
			);
			a_ByteBuffer.SkipRead(a_ByteBuffer.GetReadableSpace() - 1);
		}

		return;
	}

	// Read the plugin message and relay to clienthandle:
	AString Data;
	VERIFY(a_ByteBuffer.ReadString(Data, a_ByteBuffer.GetReadableSpace() - 1));  // Always succeeds
	m_Client->HandlePluginMessage(Channel, Data);
}





void cProtocol_1_9_0::HandlePacketSlotSelect(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEInt16, Int16, SlotNum);
	m_Client->HandleSlotSelected(SlotNum);
}





void cProtocol_1_9_0::HandlePacketSpectate(cByteBuffer & a_ByteBuffer)
{
	cUUID playerUUID;
	if (!a_ByteBuffer.ReadUUID(playerUUID))
	{
		return;
	}

	m_Client->HandleSpectate(playerUUID);
}





void cProtocol_1_9_0::HandlePacketSteerVehicle(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, Sideways);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, Forward);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, Flags);

	if ((Flags & 0x2) != 0)
	{
		m_Client->HandleUnmount();
	}
	else if ((Flags & 0x1) != 0)
	{
		// TODO: Handle vehicle jump (for animals)
	}
	else
	{
		m_Client->HandleSteerVehicle(Forward, Sideways);
	}
}





void cProtocol_1_9_0::HandlePacketTabComplete(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Text);
	HANDLE_READ(a_ByteBuffer, ReadBool,          bool,    AssumeCommand);
	HANDLE_READ(a_ByteBuffer, ReadBool,          bool,    HasPosition);

	if (HasPosition)
	{
		HANDLE_READ(a_ByteBuffer, ReadBEInt64, Int64, Position);
	}

	m_Client->HandleTabCompletion(Text);
}





void cProtocol_1_9_0::HandlePacketUpdateSign(cByteBuffer & a_ByteBuffer)
{
	int BlockX, BlockY, BlockZ;
	if (!a_ByteBuffer.ReadPosition64(BlockX, BlockY, BlockZ))
	{
		return;
	}

	AString Lines[4];
	for (int i = 0; i < 4; i++)
	{
		HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Line);
		Lines[i] = Line;
	}

	m_Client->HandleUpdateSign(BlockX, BlockY, BlockZ, Lines[0], Lines[1], Lines[2], Lines[3]);
}





void cProtocol_1_9_0::HandlePacketUseEntity(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt, UInt32, EntityID);
	HANDLE_READ(a_ByteBuffer, ReadVarInt, UInt32, Type);

	switch (Type)
	{
		case 0:
		{
			HANDLE_READ(a_ByteBuffer, ReadVarInt, UInt32, Hand);
			if (Hand == MAIN_HAND)  // TODO: implement handling of off-hand actions; ignore them for now to avoid processing actions twice
			{
				m_Client->HandleUseEntity(EntityID, false);
			}
			break;
		}
		case 1:
		{
			m_Client->HandleUseEntity(EntityID, true);
			break;
		}
		case 2:
		{
			HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, TargetX);
			HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, TargetY);
			HANDLE_READ(a_ByteBuffer, ReadBEFloat, float, TargetZ);
			HANDLE_READ(a_ByteBuffer, ReadVarInt, UInt32, Hand);

			// TODO: Do anything
			break;
		}
		default:
		{
			ASSERT(!"Unhandled use entity type!");
			return;
		}
	}
}





void cProtocol_1_9_0::HandlePacketUseItem(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadVarInt, Int32, Hand);

	m_Client->HandleUseItem(HandIntToEnum(Hand));
}





void cProtocol_1_9_0::HandlePacketEnchantItem(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, WindowID);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, Enchantment);

	m_Client->HandleEnchantItem(WindowID, Enchantment);
}





void cProtocol_1_9_0::HandlePacketVehicleMove(cByteBuffer & a_ByteBuffer)
{
	// This handles updating the vehicles location server side
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, xPos);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, yPos);
	HANDLE_READ(a_ByteBuffer, ReadBEDouble, double, zPos);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat,  float,  yaw);
	HANDLE_READ(a_ByteBuffer, ReadBEFloat,  float,  pitch);

	// Get the players vehicle
	cEntity * Vehicle = m_Client->GetPlayer()->GetAttached();

	if (Vehicle)
	{
		Vehicle->SetPosX(xPos);
		Vehicle->SetPosY(yPos);
		Vehicle->SetPosZ(zPos);
		Vehicle->SetYaw(yaw);
		Vehicle->SetPitch(pitch);
	}
}





void cProtocol_1_9_0::HandlePacketWindowClick(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8,  UInt8,  WindowID);
	HANDLE_READ(a_ByteBuffer, ReadBEInt16,  Int16,  SlotNum);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8,  UInt8,  Button);
	HANDLE_READ(a_ByteBuffer, ReadBEUInt16, UInt16, TransactionID);
	HANDLE_READ(a_ByteBuffer, ReadVarInt32,  UInt32,  Mode);
	cItem Item;
	ReadItem(a_ByteBuffer, Item);

	// Convert Button, Mode, SlotNum and HeldItem into eClickAction:
	eClickAction Action;
	switch ((Mode << 8) | Button)
	{
		case 0x0000: Action = (SlotNum != SLOT_NUM_OUTSIDE) ? caLeftClick  : caLeftClickOutside;  break;
		case 0x0001: Action = (SlotNum != SLOT_NUM_OUTSIDE) ? caRightClick : caRightClickOutside; break;
		case 0x0100: Action = caShiftLeftClick;  break;
		case 0x0101: Action = caShiftRightClick; break;
		case 0x0200: Action = caNumber1;         break;
		case 0x0201: Action = caNumber2;         break;
		case 0x0202: Action = caNumber3;         break;
		case 0x0203: Action = caNumber4;         break;
		case 0x0204: Action = caNumber5;         break;
		case 0x0205: Action = caNumber6;         break;
		case 0x0206: Action = caNumber7;         break;
		case 0x0207: Action = caNumber8;         break;
		case 0x0208: Action = caNumber9;         break;
		case 0x0302: Action = caMiddleClick;     break;
		case 0x0400: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caLeftClickOutsideHoldNothing  : caDropKey;     break;
		case 0x0401: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caRightClickOutsideHoldNothing : caCtrlDropKey; break;
		case 0x0500: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caLeftPaintBegin               : caUnknown;     break;
		case 0x0501: Action = (SlotNum != SLOT_NUM_OUTSIDE) ? caLeftPaintProgress            : caUnknown;     break;
		case 0x0502: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caLeftPaintEnd                 : caUnknown;     break;
		case 0x0504: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caRightPaintBegin              : caUnknown;     break;
		case 0x0505: Action = (SlotNum != SLOT_NUM_OUTSIDE) ? caRightPaintProgress           : caUnknown;     break;
		case 0x0506: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caRightPaintEnd                : caUnknown;     break;
		case 0x0508: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caMiddlePaintBegin             : caUnknown;     break;
		case 0x0509: Action = (SlotNum != SLOT_NUM_OUTSIDE) ? caMiddlePaintProgress          : caUnknown;     break;
		case 0x050a: Action = (SlotNum == SLOT_NUM_OUTSIDE) ? caMiddlePaintEnd               : caUnknown;     break;
		case 0x0600: Action = caDblClick; break;
		default:
		{
			LOGWARNING("Unhandled window click mode / button combination: %d (0x%x)", (Mode << 8) | Button, (Mode << 8) | Button);
			Action = caUnknown;
			break;
		}
	}

	m_Client->HandleWindowClick(WindowID, SlotNum, Action, Item);
}





void cProtocol_1_9_0::HandlePacketWindowClose(cByteBuffer & a_ByteBuffer)
{
	HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, WindowID);
	m_Client->HandleWindowClose(WindowID);
}





void cProtocol_1_9_0::HandleVanillaPluginMessage(cByteBuffer & a_ByteBuffer, const AString & a_Channel)
{
	if (a_Channel == "MC|AdvCdm")
	{
		HANDLE_READ(a_ByteBuffer, ReadBEUInt8, UInt8, Mode);
		switch (Mode)
		{
			case 0x00:
			{
				HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, BlockX);
				HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, BlockY);
				HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, BlockZ);
				HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Command);
				m_Client->HandleCommandBlockBlockChange(BlockX, BlockY, BlockZ, Command);
				break;
			}

			default:
			{
				m_Client->SendChat(Printf("Failure setting command block command; unhandled mode %u (0x%02x)", Mode, Mode), mtFailure);
				LOG("Unhandled MC|AdvCdm packet mode.");
				return;
			}
		}  // switch (Mode)
		return;
	}
	else if (a_Channel == "MC|Brand")
	{
		HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, Brand);
		m_Client->SetClientBrand(Brand);
		// Send back our brand, including the length:
		SendPluginMessage("MC|Brand", "\x08""Cuberite");
		return;
	}
	else if (a_Channel == "MC|Beacon")
	{
		HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, Effect1);
		HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, Effect2);
		m_Client->HandleBeaconSelection(Effect1, Effect2);
		return;
	}
	else if (a_Channel == "MC|ItemName")
	{
		HANDLE_READ(a_ByteBuffer, ReadVarUTF8String, AString, ItemName);
		m_Client->HandleAnvilItemName(ItemName);
		return;
	}
	else if (a_Channel == "MC|TrSel")
	{
		HANDLE_READ(a_ByteBuffer, ReadBEInt32, Int32, SlotNum);
		m_Client->HandleNPCTrade(SlotNum);
		return;
	}
	LOG("Unhandled vanilla plugin channel: \"%s\".", a_Channel.c_str());

	// Read the payload and send it through to the clienthandle:
	AString Message;
	VERIFY(a_ByteBuffer.ReadString(Message, a_ByteBuffer.GetReadableSpace() - 1));
	m_Client->HandlePluginMessage(a_Channel, Message);
}





void cProtocol_1_9_0::SendData(const char * a_Data, size_t a_Size)
{
	if (m_IsEncrypted)
	{
		Byte Encrypted[8192];  // Larger buffer, we may be sending lots of data (chunks)
		while (a_Size > 0)
		{
			size_t NumBytes = (a_Size > sizeof(Encrypted)) ? sizeof(Encrypted) : a_Size;
			m_Encryptor.ProcessData(Encrypted, reinterpret_cast<const Byte *>(a_Data), NumBytes);
			m_Client->SendData(reinterpret_cast<const char *>(Encrypted), NumBytes);
			a_Size -= NumBytes;
			a_Data += NumBytes;
		}
	}
	else
	{
		m_Client->SendData(a_Data, a_Size);
	}
}





bool cProtocol_1_9_0::ReadItem(cByteBuffer & a_ByteBuffer, cItem & a_Item, size_t a_KeepRemainingBytes)
{
	HANDLE_PACKET_READ(a_ByteBuffer, ReadBEInt16, Int16, ItemType);
	if (ItemType == -1)
	{
		// The item is empty, no more data follows
		a_Item.Empty();
		return true;
	}
	a_Item.m_ItemType = ItemType;

	HANDLE_PACKET_READ(a_ByteBuffer, ReadBEInt8,  Int8,  ItemCount);
	HANDLE_PACKET_READ(a_ByteBuffer, ReadBEInt16, Int16, ItemDamage);
	a_Item.m_ItemCount  = ItemCount;
	a_Item.m_ItemDamage = ItemDamage;
	if (ItemCount <= 0)
	{
		a_Item.Empty();
	}

	AString Metadata;
	if (!a_ByteBuffer.ReadString(Metadata, a_ByteBuffer.GetReadableSpace() - a_KeepRemainingBytes - 1) || (Metadata.size() == 0) || (Metadata[0] == 0))
	{
		// No metadata
		return true;
	}

	ParseItemMetadata(a_Item, Metadata);
	return true;
}





void cProtocol_1_9_0::ParseItemMetadata(cItem & a_Item, const AString & a_Metadata)
{
	// Parse into NBT:
	cParsedNBT NBT(a_Metadata.data(), a_Metadata.size());
	if (!NBT.IsValid())
	{
		AString HexDump;
		CreateHexDump(HexDump, a_Metadata.data(), std::max<size_t>(a_Metadata.size(), 1024), 16);
		LOGWARNING("Cannot parse NBT item metadata: %s at (%zu / %zu bytes)\n%s",
			NBT.GetErrorCode().message().c_str(), NBT.GetErrorPos(), a_Metadata.size(), HexDump.c_str()
		);
		return;
	}

	// Load enchantments and custom display names from the NBT data:
	for (int tag = NBT.GetFirstChild(NBT.GetRoot()); tag >= 0; tag = NBT.GetNextSibling(tag))
	{
		AString TagName = NBT.GetName(tag);
		switch (NBT.GetType(tag))
		{
			case TAG_List:
			{
				if ((TagName == "ench") || (TagName == "StoredEnchantments"))  // Enchantments tags
				{
					EnchantmentSerializer::ParseFromNBT(a_Item.m_Enchantments, NBT, tag);
				}
				break;
			}
			case TAG_Compound:
			{
				if (TagName == "display")  // Custom name and lore tag
				{
					for (int displaytag = NBT.GetFirstChild(tag); displaytag >= 0; displaytag = NBT.GetNextSibling(displaytag))
					{
						if ((NBT.GetType(displaytag) == TAG_String) && (NBT.GetName(displaytag) == "Name"))  // Custon name tag
						{
							a_Item.m_CustomName = NBT.GetString(displaytag);
						}
						else if ((NBT.GetType(displaytag) == TAG_List) && (NBT.GetName(displaytag) == "Lore"))  // Lore tag
						{
							a_Item.m_LoreTable.clear();
							for (int loretag = NBT.GetFirstChild(displaytag); loretag >= 0; loretag = NBT.GetNextSibling(loretag))  // Loop through array of strings
							{
								a_Item.m_LoreTable.push_back(NBT.GetString(loretag));
							}
						}
						else if ((NBT.GetType(displaytag) == TAG_Int) && (NBT.GetName(displaytag) == "color"))
						{
							a_Item.m_ItemColor.m_Color = static_cast<unsigned int>(NBT.GetInt(displaytag));
						}
					}
				}
				else if ((TagName == "Fireworks") || (TagName == "Explosion"))
				{
					cFireworkItem::ParseFromNBT(a_Item.m_FireworkItem, NBT, tag, static_cast<ENUM_ITEM_ID>(a_Item.m_ItemType));
				}
				else if (TagName == "EntityTag")
				{
					for (int entitytag = NBT.GetFirstChild(tag); entitytag >= 0; entitytag = NBT.GetNextSibling(entitytag))
					{
						if ((NBT.GetType(entitytag) == TAG_String) && (NBT.GetName(entitytag) == "id"))
						{
							AString NBTName = NBT.GetString(entitytag);
							ReplaceString(NBTName, "minecraft:", "");
							eMonsterType MonsterType = cMonster::StringToMobType(NBTName);
							a_Item.m_ItemDamage = static_cast<short>(MonsterType);

						}
					}
				}
				break;
			}
			case TAG_Int:
			{
				if (TagName == "RepairCost")
				{
					a_Item.m_RepairCost = NBT.GetInt(tag);
				}
				break;
			}
			case TAG_String:
			{
				if (TagName == "Potion")
				{
					AString PotionEffect = NBT.GetString(tag);
					if (PotionEffect.find("minecraft:") == AString::npos)
					{
						LOGD("Unknown or missing domain on potion effect name %s!", PotionEffect.c_str());
						continue;
					}

					if (PotionEffect.find("water") != AString::npos)
					{
						a_Item.m_ItemDamage = 0;
						// Water bottles shouldn't have other bits set on them; exit early.
						continue;
					}
					if (PotionEffect.find("empty") != AString::npos)
					{
						a_Item.m_ItemDamage = 0;
					}
					else if (PotionEffect.find("mundane") != AString::npos)
					{
						a_Item.m_ItemDamage = 0;
					}
					else if (PotionEffect.find("thick") != AString::npos)
					{
						a_Item.m_ItemDamage = 32;
					}
					else if (PotionEffect.find("awkward") != AString::npos)
					{
						a_Item.m_ItemDamage = 16;
					}
					else if (PotionEffect.find("regeneration") != AString::npos)
					{
						a_Item.m_ItemDamage = 1;
					}
					else if (PotionEffect.find("swiftness") != AString::npos)
					{
						a_Item.m_ItemDamage = 2;
					}
					else if (PotionEffect.find("fire_resistance") != AString::npos)
					{
						a_Item.m_ItemDamage = 3;
					}
					else if (PotionEffect.find("poison") != AString::npos)
					{
						a_Item.m_ItemDamage = 4;
					}
					else if (PotionEffect.find("healing") != AString::npos)
					{
						a_Item.m_ItemDamage = 5;
					}
					else if (PotionEffect.find("night_vision") != AString::npos)
					{
						a_Item.m_ItemDamage = 6;
					}
					else if (PotionEffect.find("weakness") != AString::npos)
					{
						a_Item.m_ItemDamage = 8;
					}
					else if (PotionEffect.find("strength") != AString::npos)
					{
						a_Item.m_ItemDamage = 9;
					}
					else if (PotionEffect.find("slowness") != AString::npos)
					{
						a_Item.m_ItemDamage = 10;
					}
					else if (PotionEffect.find("leaping") != AString::npos)
					{
						a_Item.m_ItemDamage = 11;
					}
					else if (PotionEffect.find("harming") != AString::npos)
					{
						a_Item.m_ItemDamage = 12;
					}
					else if (PotionEffect.find("water_breathing") != AString::npos)
					{
						a_Item.m_ItemDamage = 13;
					}
					else if (PotionEffect.find("invisibility") != AString::npos)
					{
						a_Item.m_ItemDamage = 14;
					}
					else
					{
						// Note: luck potions are not handled and will reach this location
						LOGD("Unknown potion type for effect name %s!", PotionEffect.c_str());
						continue;
					}

					if (PotionEffect.find("strong") != AString::npos)
					{
						a_Item.m_ItemDamage |= 0x20;
					}
					if (PotionEffect.find("long") != AString::npos)
					{
						a_Item.m_ItemDamage |= 0x40;
					}

					// Ugly special case with the changed splash potion ID in 1.9
					if ((a_Item.m_ItemType == 438) || (a_Item.m_ItemType == 441))
					{
						// Splash or lingering potions - change the ID to the normal one and mark as splash potions
						a_Item.m_ItemType = E_ITEM_POTION;
						a_Item.m_ItemDamage |= 0x4000;  // Is splash potion
					}
					else
					{
						a_Item.m_ItemDamage |= 0x2000;  // Is drinkable
					}
				}
				break;
			}
			default: LOGD("Unimplemented NBT data when parsing!"); break;
		}
	}
}





void cProtocol_1_9_0::StartEncryption(const Byte * a_Key)
{
	m_Encryptor.Init(a_Key, a_Key);
	m_Decryptor.Init(a_Key, a_Key);
	m_IsEncrypted = true;

	// Prepare the m_AuthServerID:
	cSha1Checksum Checksum;
	cServer * Server = cRoot::Get()->GetServer();
	const AString & ServerID = Server->GetServerID();
	Checksum.Update(reinterpret_cast<const Byte *>(ServerID.c_str()), ServerID.length());
	Checksum.Update(a_Key, 16);
	Checksum.Update(reinterpret_cast<const Byte *>(Server->GetPublicKeyDER().data()), Server->GetPublicKeyDER().size());
	Byte Digest[20];
	Checksum.Finalize(Digest);
	cSha1Checksum::DigestToJava(Digest, m_AuthServerID);
}





eBlockFace cProtocol_1_9_0::FaceIntToBlockFace(Int32 a_BlockFace)
{
	// Normalize the blockface values returned from the protocol
	// Anything known gets mapped 1:1, everything else returns BLOCK_FACE_NONE
	switch (a_BlockFace)
	{
		case BLOCK_FACE_XM: return BLOCK_FACE_XM;
		case BLOCK_FACE_XP: return BLOCK_FACE_XP;
		case BLOCK_FACE_YM: return BLOCK_FACE_YM;
		case BLOCK_FACE_YP: return BLOCK_FACE_YP;
		case BLOCK_FACE_ZM: return BLOCK_FACE_ZM;
		case BLOCK_FACE_ZP: return BLOCK_FACE_ZP;
		default: return BLOCK_FACE_NONE;
	}
}





eHand cProtocol_1_9_0::HandIntToEnum(Int32 a_Hand)
{
	// Convert hand parameter into eHand enum
	switch (a_Hand)
	{
		case MAIN_HAND: return eHand::hMain;
		case OFF_HAND: return eHand::hOff;
		default:
		{
			ASSERT(!"Unknown hand value");
			return eHand::hMain;
		}
	}
}





void cProtocol_1_9_0::SendPacket(cPacketizer & a_Pkt)
{
	UInt32 PacketLen = static_cast<UInt32>(m_OutPacketBuffer.GetUsedSpace());
	AString PacketData, CompressedPacket;
	m_OutPacketBuffer.ReadAll(PacketData);
	m_OutPacketBuffer.CommitRead();

	if ((m_State == 3) && (PacketLen >= 256))
	{
		// Compress the packet payload:
		if (!cProtocol_1_9_0::CompressPacket(PacketData, CompressedPacket))
		{
			return;
		}
	}
	else if (m_State == 3)
	{
		// The packet is not compressed, indicate this in the packet header:
		m_OutPacketLenBuffer.WriteVarInt32(PacketLen + 1);
		m_OutPacketLenBuffer.WriteVarInt32(0);
		AString LengthData;
		m_OutPacketLenBuffer.ReadAll(LengthData);
		SendData(LengthData.data(), LengthData.size());
	}
	else
	{
		// Compression doesn't apply to this state, send raw data:
		m_OutPacketLenBuffer.WriteVarInt32(PacketLen);
		AString LengthData;
		m_OutPacketLenBuffer.ReadAll(LengthData);
		SendData(LengthData.data(), LengthData.size());
	}

	// Send the packet's payload, either direct or compressed:
	if (CompressedPacket.empty())
	{
		m_OutPacketLenBuffer.CommitRead();
		SendData(PacketData.data(), PacketData.size());
	}
	else
	{
		SendData(CompressedPacket.data(), CompressedPacket.size());
	}

	// Log the comm into logfile:
	if (g_ShouldLogCommOut && m_CommLogFile.IsOpen())
	{
		AString Hex;
		ASSERT(PacketData.size() > 0);
		CreateHexDump(Hex, PacketData.data(), PacketData.size(), 16);
		m_CommLogFile.Printf("Outgoing packet: type %s (translated to 0x%02x), length %u (0x%04x), state %d. Payload (incl. type):\n%s\n",
			cPacketizer::PacketTypeToStr(a_Pkt.GetPacketType()), GetPacketID(a_Pkt.GetPacketType()),
			PacketLen, PacketLen, m_State, Hex
		);
		/*
		// Useful for debugging a new protocol:
		LOGD("Outgoing packet: type %s (translated to 0x%02x), length %u (0x%04x), state %d. Payload (incl. type):\n%s\n",
			cPacketizer::PacketTypeToStr(a_Pkt.GetPacketType()), GetPacketID(a_Pkt.GetPacketType()),
			PacketLen, PacketLen, m_State, Hex
		);
		//*/
	}
	/*
	// Useful for debugging a new protocol:
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	*/
}





void cProtocol_1_9_0::WriteItem(cPacketizer & a_Pkt, const cItem & a_Item)
{
	short ItemType = a_Item.m_ItemType;
	ASSERT(ItemType >= -1);  // Check validity of packets in debug runtime
	if (ItemType <= 0)
	{
		// Fix, to make sure no invalid values are sent.
		ItemType = -1;
	}

	if (a_Item.IsEmpty())
	{
		a_Pkt.WriteBEInt16(-1);
		return;
	}

	if ((ItemType == E_ITEM_POTION) && ((a_Item.m_ItemDamage & 0x4000) != 0))
	{
		// Ugly special case for splash potion ids which changed in 1.9; this can be removed when the new 1.9 ids are implemented
		a_Pkt.WriteBEInt16(438);  // minecraft:splash_potion
	}
	else
	{
		// Normal item
		a_Pkt.WriteBEInt16(ItemType);
	}
	a_Pkt.WriteBEInt8(a_Item.m_ItemCount);
	if ((ItemType == E_ITEM_POTION) || (ItemType == E_ITEM_SPAWN_EGG))
	{
		// These items lost their metadata; if it is sent they don't render correctly.
		a_Pkt.WriteBEInt16(0);
	}
	else
	{
		a_Pkt.WriteBEInt16(a_Item.m_ItemDamage);
	}

	if (a_Item.m_Enchantments.IsEmpty() && a_Item.IsBothNameAndLoreEmpty() && (ItemType != E_ITEM_FIREWORK_ROCKET) && (ItemType != E_ITEM_FIREWORK_STAR) && !a_Item.m_ItemColor.IsValid() && (ItemType != E_ITEM_POTION) && (ItemType != E_ITEM_SPAWN_EGG))
	{
		a_Pkt.WriteBEInt8(0);
		return;
	}


	// Send the enchantments and custom names:
	cFastNBTWriter Writer;
	if (a_Item.m_RepairCost != 0)
	{
		Writer.AddInt("RepairCost", a_Item.m_RepairCost);
	}
	if (!a_Item.m_Enchantments.IsEmpty())
	{
		const char * TagName = (a_Item.m_ItemType == E_ITEM_BOOK) ? "StoredEnchantments" : "ench";
		EnchantmentSerializer::WriteToNBTCompound(a_Item.m_Enchantments, Writer, TagName);
	}
	if (!a_Item.IsBothNameAndLoreEmpty() || a_Item.m_ItemColor.IsValid())
	{
		Writer.BeginCompound("display");
		if (a_Item.m_ItemColor.IsValid())
		{
			Writer.AddInt("color", static_cast<Int32>(a_Item.m_ItemColor.m_Color));
		}

		if (!a_Item.IsCustomNameEmpty())
		{
			Writer.AddString("Name", a_Item.m_CustomName.c_str());
		}
		if (!a_Item.IsLoreEmpty())
		{
			Writer.BeginList("Lore", TAG_String);

			for (const auto & Line : a_Item.m_LoreTable)
			{
				Writer.AddString("", Line);
			}

			Writer.EndList();
		}
		Writer.EndCompound();
	}
	if ((a_Item.m_ItemType == E_ITEM_FIREWORK_ROCKET) || (a_Item.m_ItemType == E_ITEM_FIREWORK_STAR))
	{
		cFireworkItem::WriteToNBTCompound(a_Item.m_FireworkItem, Writer, static_cast<ENUM_ITEM_ID>(a_Item.m_ItemType));
	}
	if (a_Item.m_ItemType == E_ITEM_POTION)
	{
		// 1.9 potions use a different format.  In the future (when only 1.9+ is supported) this should be its own class
		AString PotionID = "empty";  // Fallback of "Uncraftable potion" for unhandled cases

		cEntityEffect::eType Type = cEntityEffect::GetPotionEffectType(a_Item.m_ItemDamage);
		if (Type != cEntityEffect::effNoEffect)
		{
			switch (Type)
			{
				case cEntityEffect::effRegeneration: PotionID = "regeneration"; break;
				case cEntityEffect::effSpeed: PotionID = "swiftness"; break;
				case cEntityEffect::effFireResistance: PotionID = "fire_resistance"; break;
				case cEntityEffect::effPoison: PotionID = "poison"; break;
				case cEntityEffect::effInstantHealth: PotionID = "healing"; break;
				case cEntityEffect::effNightVision: PotionID = "night_vision"; break;
				case cEntityEffect::effWeakness: PotionID = "weakness"; break;
				case cEntityEffect::effStrength: PotionID = "strength"; break;
				case cEntityEffect::effSlowness: PotionID = "slowness"; break;
				case cEntityEffect::effJumpBoost: PotionID = "leaping"; break;
				case cEntityEffect::effInstantDamage: PotionID = "harming"; break;
				case cEntityEffect::effWaterBreathing: PotionID = "water_breathing"; break;
				case cEntityEffect::effInvisibility: PotionID = "invisibility"; break;
				default: ASSERT(!"Unknown potion effect"); break;
			}
			if (cEntityEffect::GetPotionEffectIntensity(a_Item.m_ItemDamage) == 1)
			{
				PotionID = "strong_" + PotionID;
			}
			else if (a_Item.m_ItemDamage & 0x40)
			{
				// Extended potion bit
				PotionID = "long_" + PotionID;
			}
		}
		else
		{
			// Empty potions: Water bottles and other base ones
			if (a_Item.m_ItemDamage == 0)
			{
				// No other bits set; thus it's a water bottle
				PotionID = "water";
			}
			else
			{
				switch (a_Item.m_ItemDamage & 0x3f)
				{
					case 0x00: PotionID = "mundane"; break;
					case 0x10: PotionID = "awkward"; break;
					case 0x20: PotionID = "thick"; break;
				}
				// Default cases will use "empty" from before.
			}
		}

		PotionID = "minecraft:" + PotionID;

		Writer.AddString("Potion", PotionID.c_str());
	}
	if (a_Item.m_ItemType == E_ITEM_SPAWN_EGG)
	{
		// Convert entity ID to the name.
		eMonsterType MonsterType = cItemSpawnEggHandler::ItemDamageToMonsterType(a_Item.m_ItemDamage);
		if (MonsterType != eMonsterType::mtInvalidType)
		{
			Writer.BeginCompound("EntityTag");
			Writer.AddString("id", "minecraft:" + cMonster::MobTypeToVanillaNBT(MonsterType));
			Writer.EndCompound();
		}
	}

	Writer.Finish();

	AString Result = Writer.GetResult();
	if (Result.size() == 0)
	{
		a_Pkt.WriteBEInt8(0);
		return;
	}
	a_Pkt.WriteBuf(Result.data(), Result.size());
}





void cProtocol_1_9_0::WriteBlockEntity(cPacketizer & a_Pkt, const cBlockEntity & a_BlockEntity)
{
	cFastNBTWriter Writer;

	switch (a_BlockEntity.GetBlockType())
	{
		case E_BLOCK_BEACON:
		{
			auto & BeaconEntity = static_cast<const cBeaconEntity &>(a_BlockEntity);
			Writer.AddInt("x",         BeaconEntity.GetPosX());
			Writer.AddInt("y",         BeaconEntity.GetPosY());
			Writer.AddInt("z",         BeaconEntity.GetPosZ());
			Writer.AddInt("Primary",   BeaconEntity.GetPrimaryEffect());
			Writer.AddInt("Secondary", BeaconEntity.GetSecondaryEffect());
			Writer.AddInt("Levels",    BeaconEntity.GetBeaconLevel());
			Writer.AddString("id", "Beacon");  // "Tile Entity ID" - MC wiki; vanilla server always seems to send this though
			break;
		}

		case E_BLOCK_COMMAND_BLOCK:
		{
			auto & CommandBlockEntity = static_cast<const cCommandBlockEntity &>(a_BlockEntity);
			Writer.AddByte("TrackOutput", 1);  // Neither I nor the MC wiki has any idea about this
			Writer.AddInt("SuccessCount", CommandBlockEntity.GetResult());
			Writer.AddInt("x", CommandBlockEntity.GetPosX());
			Writer.AddInt("y", CommandBlockEntity.GetPosY());
			Writer.AddInt("z", CommandBlockEntity.GetPosZ());
			Writer.AddString("Command", CommandBlockEntity.GetCommand().c_str());
			// You can set custom names for windows in Vanilla
			// For a command block, this would be the 'name' prepended to anything it outputs into global chat
			// MCS doesn't have this, so just leave it @ '@'. (geddit?)
			Writer.AddString("CustomName", "@");
			Writer.AddString("id", "Control");  // "Tile Entity ID" - MC wiki; vanilla server always seems to send this though
			if (!CommandBlockEntity.GetLastOutput().empty())
			{
				Writer.AddString("LastOutput", Printf("{\"text\":\"%s\"}", CommandBlockEntity.GetLastOutput().c_str()));
			}
			break;
		}

		case E_BLOCK_HEAD:
		{
			auto & MobHeadEntity = static_cast<const cMobHeadEntity &>(a_BlockEntity);
			Writer.AddInt("x", MobHeadEntity.GetPosX());
			Writer.AddInt("y", MobHeadEntity.GetPosY());
			Writer.AddInt("z", MobHeadEntity.GetPosZ());
			Writer.AddByte("SkullType", MobHeadEntity.GetType() & 0xFF);
			Writer.AddByte("Rot", MobHeadEntity.GetRotation() & 0xFF);
			Writer.AddString("id", "Skull");  // "Tile Entity ID" - MC wiki; vanilla server always seems to send this though

			// The new Block Entity format for a Mob Head. See: https://minecraft.gamepedia.com/Head#Block_entity
			Writer.BeginCompound("Owner");
				Writer.AddString("Id", MobHeadEntity.GetOwnerUUID().ToShortString());
				Writer.AddString("Name", MobHeadEntity.GetOwnerName());
				Writer.BeginCompound("Properties");
					Writer.BeginList("textures", TAG_Compound);
						Writer.BeginCompound("");
							Writer.AddString("Signature", MobHeadEntity.GetOwnerTextureSignature());
							Writer.AddString("Value", MobHeadEntity.GetOwnerTexture());
						Writer.EndCompound();
					Writer.EndList();
				Writer.EndCompound();
			Writer.EndCompound();
			break;
		}

		case E_BLOCK_FLOWER_POT:
		{
			auto & FlowerPotEntity = static_cast<const cFlowerPotEntity &>(a_BlockEntity);
			Writer.AddInt("x", FlowerPotEntity.GetPosX());
			Writer.AddInt("y", FlowerPotEntity.GetPosY());
			Writer.AddInt("z", FlowerPotEntity.GetPosZ());
			Writer.AddInt("Item", static_cast<Int32>(FlowerPotEntity.GetItem().m_ItemType));
			Writer.AddInt("Data", static_cast<Int32>(FlowerPotEntity.GetItem().m_ItemDamage));
			Writer.AddString("id", "FlowerPot");  // "Tile Entity ID" - MC wiki; vanilla server always seems to send this though
			break;
		}

		case E_BLOCK_MOB_SPAWNER:
		{
			auto & MobSpawnerEntity = static_cast<const cMobSpawnerEntity &>(a_BlockEntity);
			Writer.AddInt("x", MobSpawnerEntity.GetPosX());
			Writer.AddInt("y", MobSpawnerEntity.GetPosY());
			Writer.AddInt("z", MobSpawnerEntity.GetPosZ());
			Writer.BeginCompound("SpawnData");
				Writer.AddString("id", cMonster::MobTypeToVanillaName(MobSpawnerEntity.GetEntity()));
			Writer.EndCompound();
			Writer.AddShort("Delay", MobSpawnerEntity.GetSpawnDelay());
			Writer.AddString("id", "MobSpawner");
			break;
		}

		default:
		{
			break;
		}
	}

	Writer.Finish();
	a_Pkt.WriteBuf(Writer.GetResult().data(), Writer.GetResult().size());
}





void cProtocol_1_9_0::WriteEntityMetadata(cPacketizer & a_Pkt, const cEntity & a_Entity)
{
	// Common metadata:
	Int8 Flags = 0;
	if (a_Entity.IsOnFire())
	{
		Flags |= 0x01;
	}
	if (a_Entity.IsCrouched())
	{
		Flags |= 0x02;
	}
	if (a_Entity.IsSprinting())
	{
		Flags |= 0x08;
	}
	if (a_Entity.IsRclking())
	{
		Flags |= 0x10;
	}
	if (a_Entity.IsInvisible())
	{
		Flags |= 0x20;
	}
	a_Pkt.WriteBEUInt8(0);  // Index 0
	a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);  // Type
	a_Pkt.WriteBEInt8(Flags);

	switch (a_Entity.GetEntityType())
	{
		case cEntity::etPlayer:
		{
			auto & Player = static_cast<const cPlayer &>(a_Entity);

			// TODO Set player custom name to their name.
			// Then it's possible to move the custom name of mobs to the entities
			// and to remove the "special" player custom name.
			a_Pkt.WriteBEUInt8(2);  // Index 2: Custom name
			a_Pkt.WriteBEUInt8(METADATA_TYPE_STRING);
			a_Pkt.WriteString(Player.GetName());

			a_Pkt.WriteBEUInt8(6);  // Index 6: Health
			a_Pkt.WriteBEUInt8(METADATA_TYPE_FLOAT);
			a_Pkt.WriteBEFloat(static_cast<float>(Player.GetHealth()));

			a_Pkt.WriteBEUInt8(12);
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			a_Pkt.WriteBEUInt8(static_cast<UInt8>(Player.GetSkinParts()));

			a_Pkt.WriteBEUInt8(13);
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			a_Pkt.WriteBEUInt8(static_cast<UInt8>(Player.GetMainHand()));
			break;
		}
		case cEntity::etPickup:
		{
			a_Pkt.WriteBEUInt8(5);  // Index 5: Item
			a_Pkt.WriteBEUInt8(METADATA_TYPE_ITEM);
			WriteItem(a_Pkt, static_cast<const cPickup &>(a_Entity).GetItem());
			break;
		}
		case cEntity::etMinecart:
		{
			a_Pkt.WriteBEUInt8(5);  // Index 5: Shaking power
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);

			// The following expression makes Minecarts shake more with less health or higher damage taken
			auto & Minecart = static_cast<const cMinecart &>(a_Entity);
			auto maxHealth = a_Entity.GetMaxHealth();
			auto curHealth = a_Entity.GetHealth();
			a_Pkt.WriteVarInt32(static_cast<UInt32>((maxHealth - curHealth) * Minecart.LastDamage() * 4));

			a_Pkt.WriteBEUInt8(6);  // Index 6: Shaking direction (doesn't seem to effect anything)
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(1);

			a_Pkt.WriteBEUInt8(7);  // Index 7: Shake multiplier / damage taken
			a_Pkt.WriteBEUInt8(METADATA_TYPE_FLOAT);
			a_Pkt.WriteBEFloat(static_cast<float>(Minecart.LastDamage() + 10));

			if (Minecart.GetPayload() == cMinecart::mpNone)
			{
				auto & RideableMinecart = static_cast<const cRideableMinecart &>(Minecart);
				const cItem & MinecartContent = RideableMinecart.GetContent();
				if (!MinecartContent.IsEmpty())
				{
					a_Pkt.WriteBEUInt8(8);  // Index 8: Block ID and damage
					a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
					int Content = MinecartContent.m_ItemType;
					Content |= MinecartContent.m_ItemDamage << 8;
					a_Pkt.WriteVarInt32(static_cast<UInt32>(Content));

					a_Pkt.WriteBEUInt8(9);  // Index 9: Block ID and damage
					a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
					a_Pkt.WriteVarInt32(static_cast<UInt32>(RideableMinecart.GetBlockHeight()));

					a_Pkt.WriteBEUInt8(10);  // Index 10: Show block
					a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
					a_Pkt.WriteBool(true);
				}
			}
			else if (Minecart.GetPayload() == cMinecart::mpFurnace)
			{
				a_Pkt.WriteBEUInt8(11);  // Index 11: Is powered
				a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
				a_Pkt.WriteBool(static_cast<const cMinecartWithFurnace &>(Minecart).IsFueled());
			}
			break;
		}  // case etMinecart

		case cEntity::etProjectile:
		{
			auto & Projectile = static_cast<const cProjectileEntity &>(a_Entity);
			switch (Projectile.GetProjectileKind())
			{
				case cProjectileEntity::pkArrow:
				{
					a_Pkt.WriteBEUInt8(5);  // Index 5: Is critical
					a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
					a_Pkt.WriteBEInt8(static_cast<const cArrowEntity &>(Projectile).IsCritical() ? 1 : 0);
					break;
				}
				case cProjectileEntity::pkFirework:
				{
					a_Pkt.WriteBEUInt8(5);  // Index 5: Firework item used for this firework
					a_Pkt.WriteBEUInt8(METADATA_TYPE_ITEM);
					WriteItem(a_Pkt, static_cast<const cFireworkEntity &>(Projectile).GetItem());
					break;
				}
				case cProjectileEntity::pkSplashPotion:
				{
					a_Pkt.WriteBEUInt8(5);  // Index 5: Potion item which was thrown
					a_Pkt.WriteBEUInt8(METADATA_TYPE_ITEM);
					WriteItem(a_Pkt, static_cast<const cSplashPotionEntity &>(Projectile).GetItem());
				}
				default:
				{
					break;
				}
			}
			break;
		}  // case etProjectile

		case cEntity::etMonster:
		{
			WriteMobMetadata(a_Pkt, static_cast<const cMonster &>(a_Entity));
			break;
		}

		case cEntity::etBoat:
		{
			auto & Boat = static_cast<const cBoat &>(a_Entity);

			a_Pkt.WriteBEInt8(5);  // Index 6: Time since last hit
			a_Pkt.WriteBEInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Boat.GetLastDamage()));

			a_Pkt.WriteBEInt8(6);  // Index 7: Forward direction
			a_Pkt.WriteBEInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Boat.GetForwardDirection()));

			a_Pkt.WriteBEInt8(7);  // Index 8: Damage taken
			a_Pkt.WriteBEInt8(METADATA_TYPE_FLOAT);
			a_Pkt.WriteBEFloat(Boat.GetDamageTaken());

			a_Pkt.WriteBEInt8(8);  // Index 9: Type
			a_Pkt.WriteBEInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Boat.GetMaterial()));

			a_Pkt.WriteBEInt8(9);  // Index 10: Right paddle turning
			a_Pkt.WriteBEInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Boat.IsRightPaddleUsed());

			a_Pkt.WriteBEInt8(10);  // Index 11: Left paddle turning
			a_Pkt.WriteBEInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Boat.IsLeftPaddleUsed());

			break;
		}  // case etBoat

		case cEntity::etItemFrame:
		{
			auto & Frame = static_cast<const cItemFrame &>(a_Entity);
			a_Pkt.WriteBEUInt8(5);  // Index 5: Item
			a_Pkt.WriteBEUInt8(METADATA_TYPE_ITEM);
			WriteItem(a_Pkt, Frame.GetItem());
			a_Pkt.WriteBEUInt8(6);  // Index 6: Rotation
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(Frame.GetItemRotation());
			break;
		}  // case etItemFrame

		default:
		{
			break;
		}
	}
}





void cProtocol_1_9_0::WriteMobMetadata(cPacketizer & a_Pkt, const cMonster & a_Mob)
{
	// Living Enitiy Metadata
	if (a_Mob.HasCustomName())
	{
		// TODO: As of 1.9 _all_ entities can have custom names; should this be moved up?
		a_Pkt.WriteBEUInt8(2);  // Index 2: Custom name
		a_Pkt.WriteBEUInt8(METADATA_TYPE_STRING);
		a_Pkt.WriteString(a_Mob.GetCustomName());

		a_Pkt.WriteBEUInt8(3);  // Index 3: Custom name always visible
		a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
		a_Pkt.WriteBool(a_Mob.IsCustomNameAlwaysVisible());
	}

	a_Pkt.WriteBEUInt8(6);  // Index 6: Health
	a_Pkt.WriteBEUInt8(METADATA_TYPE_FLOAT);
	a_Pkt.WriteBEFloat(static_cast<float>(a_Mob.GetHealth()));

	switch (a_Mob.GetMobType())
	{
		case mtBat:
		{
			auto & Bat = static_cast<const cBat &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Bat flags - currently only hanging
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			a_Pkt.WriteBEInt8(Bat.IsHanging() ? 1 : 0);
			break;
		}  // case mtBat

		case mtChicken:
		{
			auto & Chicken = static_cast<const cChicken &>(a_Mob);

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Chicken.IsBaby());
			break;
		}  // case mtChicken

		case mtCow:
		{
			auto & Cow = static_cast<const cCow &>(a_Mob);

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Cow.IsBaby());
			break;
		}  // case mtCow

		case mtCreeper:
		{
			auto & Creeper = static_cast<const cCreeper &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: State (idle or "blowing")
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(Creeper.IsBlowing() ? 1 : 0xffffffff);

			a_Pkt.WriteBEUInt8(12);  // Index 12: Is charged
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Creeper.IsCharged());

			a_Pkt.WriteBEUInt8(13);  // Index 13: Is ignited
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Creeper.IsBurnedWithFlintAndSteel());
			break;
		}  // case mtCreeper

		case mtEnderman:
		{
			auto & Enderman = static_cast<const cEnderman &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Carried block
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BLOCKID);
			UInt32 Carried = 0;
			Carried |= static_cast<UInt32>(Enderman.GetCarriedBlock() << 4);
			Carried |= Enderman.GetCarriedMeta();
			a_Pkt.WriteVarInt32(Carried);

			a_Pkt.WriteBEUInt8(12);  // Index 12: Is screaming
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Enderman.IsScreaming());
			break;
		}  // case mtEnderman

		case mtGhast:
		{
			auto & Ghast = static_cast<const cGhast &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Is attacking
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Ghast.IsCharging());
			break;
		}  // case mtGhast

		case mtHorse:
		{
			auto & Horse = static_cast<const cHorse &>(a_Mob);
			Int8 Flags = 0;
			if (Horse.IsTame())
			{
				Flags |= 0x02;
			}
			if (Horse.IsSaddled())
			{
				Flags |= 0x04;
			}
			if (Horse.IsChested())
			{
				Flags |= 0x08;
			}
			if (Horse.IsEating())
			{
				Flags |= 0x20;
			}
			if (Horse.IsRearing())
			{
				Flags |= 0x40;
			}
			if (Horse.IsMthOpen())
			{
				Flags |= 0x80;
			}
			a_Pkt.WriteBEUInt8(12);  // Index 12: flags
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			a_Pkt.WriteBEInt8(Flags);

			a_Pkt.WriteBEUInt8(13);  // Index 13: Variant / type
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Horse.GetHorseType()));

			a_Pkt.WriteBEUInt8(14);  // Index 14: Color / style
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			int Appearance = 0;
			Appearance = Horse.GetHorseColor();
			Appearance |= Horse.GetHorseStyle() << 8;
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Appearance));

			a_Pkt.WriteBEUInt8(16);  // Index 16: Armor
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Horse.GetHorseArmour()));

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Horse.IsBaby());
			break;
		}  // case mtHorse

		case mtMagmaCube:
		{
			auto & MagmaCube = static_cast<const cMagmaCube &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Size
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(MagmaCube.GetSize()));
			break;
		}  // case mtMagmaCube

		case mtOcelot:
		{
			auto & Ocelot = static_cast<const cOcelot &>(a_Mob);

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Ocelot.IsBaby());
			break;
		}  // case mtOcelot

		case mtPig:
		{
			auto & Pig = static_cast<const cPig &>(a_Mob);

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Pig.IsBaby());

			a_Pkt.WriteBEUInt8(12);  // Index 12: Is saddled
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Pig.IsSaddled());

			break;
		}  // case mtPig

		case mtRabbit:
		{
			auto & Rabbit = static_cast<const cRabbit &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Rabbit.IsBaby());

			a_Pkt.WriteBEUInt8(12);  // Index 12: Type
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Rabbit.GetRabbitType()));
			break;
		}  // case mtRabbit

		case mtSheep:
		{
			auto & Sheep = static_cast<const cSheep &>(a_Mob);

			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Sheep.IsBaby());

			a_Pkt.WriteBEUInt8(12);  // Index 12: sheared, color
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			Int8 SheepMetadata = 0;
			SheepMetadata = static_cast<Int8>(Sheep.GetFurColor());
			if (Sheep.IsSheared())
			{
				SheepMetadata |= 0x10;
			}
			a_Pkt.WriteBEInt8(SheepMetadata);
			break;
		}  // case mtSheep

		case mtSkeleton:
		{
			auto & Skeleton = static_cast<const cSkeleton &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Type
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(Skeleton.IsWither() ? 1 : 0);
			break;
		}  // case mtSkeleton

		case mtSlime:
		{
			auto & Slime = static_cast<const cSlime &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Size
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Slime.GetSize()));
			break;
		}  // case mtSlime

		case mtVillager:
		{
			auto & Villager = static_cast<const cVillager &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Villager.IsBaby());

			a_Pkt.WriteBEUInt8(12);  // Index 12: Type
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Villager.GetVilType()));
			break;
		}  // case mtVillager

		case mtWitch:
		{
			auto & Witch = static_cast<const cWitch &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is angry
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Witch.IsAngry());
			break;
		}  // case mtWitch

		case mtWither:
		{
			auto & Wither = static_cast<const cWither &>(a_Mob);
			a_Pkt.WriteBEUInt8(14);  // Index 14: Invulnerable ticks
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(Wither.GetWitherInvulnerableTicks());

			// TODO: Use boss bar packet for health
			break;
		}  // case mtWither

		case mtWolf:
		{
			auto & Wolf = static_cast<const cWolf &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Wolf.IsBaby());

			Int8 WolfStatus = 0;
			if (Wolf.IsSitting())
			{
				WolfStatus |= 0x1;
			}
			if (Wolf.IsAngry())
			{
				WolfStatus |= 0x2;
			}
			if (Wolf.IsTame())
			{
				WolfStatus |= 0x4;
			}
			a_Pkt.WriteBEUInt8(12);  // Index 12: status
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BYTE);
			a_Pkt.WriteBEInt8(WolfStatus);

			a_Pkt.WriteBEUInt8(14);  // Index 14: Health
			a_Pkt.WriteBEUInt8(METADATA_TYPE_FLOAT);
			a_Pkt.WriteBEFloat(static_cast<float>(a_Mob.GetHealth()));

			a_Pkt.WriteBEUInt8(15);  // Index 15: Is begging
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Wolf.IsBegging());

			a_Pkt.WriteBEUInt8(16);  // Index 16: Collar color
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(static_cast<UInt32>(Wolf.GetCollarColor()));
			break;
		}  // case mtWolf

		case mtZombie:
		{
			auto & Zombie = static_cast<const cZombie &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Zombie.IsBaby());

			a_Pkt.WriteBEUInt8(12);  // Index 12: Is a villager
			a_Pkt.WriteBEUInt8(METADATA_TYPE_VARINT);
			a_Pkt.WriteVarInt32(Zombie.IsVillagerZombie() ? 1 : 0);  // TODO: This actually encodes the zombie villager profession, but that isn't implemented yet.

			a_Pkt.WriteBEUInt8(13);  // Index 13: Is converting
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(Zombie.IsConverting());
			break;
		}  // case mtZombie

		case mtZombiePigman:
		{
			auto & ZombiePigman = static_cast<const cZombiePigman &>(a_Mob);
			a_Pkt.WriteBEUInt8(11);  // Index 11: Is baby
			a_Pkt.WriteBEUInt8(METADATA_TYPE_BOOL);
			a_Pkt.WriteBool(ZombiePigman.IsBaby());
			break;
		}  // case mtZombiePigman

		default: break;
	}  // switch (a_Mob.GetType())
}





void cProtocol_1_9_0::WriteEntityProperties(cPacketizer & a_Pkt, const cEntity & a_Entity)
{
	if (!a_Entity.IsMob())
	{
		// No properties for anything else than mobs
		a_Pkt.WriteBEInt32(0);
		return;
	}

	// const cMonster & Mob = (const cMonster &)a_Entity;

	// TODO: Send properties and modifiers based on the mob type

	a_Pkt.WriteBEInt32(0);  // NumProperties
}





////////////////////////////////////////////////////////////////////////////////
// cProtocol_1_9_1:

cProtocol_1_9_1::cProtocol_1_9_1(cClientHandle * a_Client, const AString & a_ServerAddress, UInt16 a_ServerPort, UInt32 a_State) :
	Super(a_Client, a_ServerAddress, a_ServerPort, a_State)
{
}





void cProtocol_1_9_1::SendLogin(const cPlayer & a_Player, const cWorld & a_World)
{
	// Send the Join Game packet:
	{
		cServer * Server = cRoot::Get()->GetServer();
		cPacketizer Pkt(*this, pktJoinGame);
		Pkt.WriteBEUInt32(a_Player.GetUniqueID());
		Pkt.WriteBEUInt8(static_cast<UInt8>(a_Player.GetEffectiveGameMode()) | (Server->IsHardcore() ? 0x08 : 0));  // Hardcore flag bit 4
		Pkt.WriteBEInt32(static_cast<Int32>(a_World.GetDimension()));  // This is the change from 1.9.0 (Int8 to Int32)
		Pkt.WriteBEUInt8(2);  // TODO: Difficulty (set to Normal)
		Pkt.WriteBEUInt8(static_cast<UInt8>(Clamp<size_t>(Server->GetMaxPlayers(), 0, 255)));
		Pkt.WriteString("default");  // Level type - wtf?
		Pkt.WriteBool(false);  // Reduced Debug Info - wtf?
	}

	// Send the spawn position:
	{
		cPacketizer Pkt(*this, pktSpawnPosition);
		Pkt.WritePosition64(FloorC(a_World.GetSpawnX()), FloorC(a_World.GetSpawnY()), FloorC(a_World.GetSpawnZ()));
	}

	// Send the server difficulty:
	{
		cPacketizer Pkt(*this, pktDifficulty);
		Pkt.WriteBEInt8(1);
	}

	// Send player abilities:
	SendPlayerAbilities();
}





void cProtocol_1_9_1::HandlePacketStatusRequest(cByteBuffer & a_ByteBuffer)
{
	cServer * Server = cRoot::Get()->GetServer();
	AString ServerDescription = Server->GetDescription();
	auto NumPlayers = static_cast<signed>(Server->GetNumPlayers());
	auto MaxPlayers = static_cast<signed>(Server->GetMaxPlayers());
	AString Favicon = Server->GetFaviconData();
	cRoot::Get()->GetPluginManager()->CallHookServerPing(*m_Client, ServerDescription, NumPlayers, MaxPlayers, Favicon);

	// Version:
	Json::Value Version;
	Version["name"] = "Cuberite 1.9.1";
	Version["protocol"] = 108;

	// Players:
	Json::Value Players;
	Players["online"] = NumPlayers;
	Players["max"] = MaxPlayers;
	// TODO: Add "sample"

	// Description:
	Json::Value Description;
	Description["text"] = ServerDescription.c_str();

	// Create the response:
	Json::Value ResponseValue;
	ResponseValue["version"] = Version;
	ResponseValue["players"] = Players;
	ResponseValue["description"] = Description;
	m_Client->ForgeAugmentServerListPing(ResponseValue);
	if (!Favicon.empty())
	{
		ResponseValue["favicon"] = Printf("data:image/png;base64,%s", Favicon.c_str());
	}

	Json::FastWriter Writer;
	AString Response = Writer.write(ResponseValue);

	cPacketizer Pkt(*this, pktStatusResponse);
	Pkt.WriteString(Response);
}





////////////////////////////////////////////////////////////////////////////////
// cProtocol_1_9_2:

cProtocol_1_9_2::cProtocol_1_9_2(cClientHandle * a_Client, const AString & a_ServerAddress, UInt16 a_ServerPort, UInt32 a_State) :
	Super(a_Client, a_ServerAddress, a_ServerPort, a_State)
{
}





void cProtocol_1_9_2::HandlePacketStatusRequest(cByteBuffer & a_ByteBuffer)
{
	cServer * Server = cRoot::Get()->GetServer();
	AString ServerDescription = Server->GetDescription();
	auto NumPlayers = static_cast<signed>(Server->GetNumPlayers());
	auto MaxPlayers = static_cast<signed>(Server->GetMaxPlayers());
	AString Favicon = Server->GetFaviconData();
	cRoot::Get()->GetPluginManager()->CallHookServerPing(*m_Client, ServerDescription, NumPlayers, MaxPlayers, Favicon);

	// Version:
	Json::Value Version;
	Version["name"] = "Cuberite 1.9.2";
	Version["protocol"] = 109;

	// Players:
	Json::Value Players;
	Players["online"] = NumPlayers;
	Players["max"] = MaxPlayers;
	// TODO: Add "sample"

	// Description:
	Json::Value Description;
	Description["text"] = ServerDescription.c_str();

	// Create the response:
	Json::Value ResponseValue;
	ResponseValue["version"] = Version;
	ResponseValue["players"] = Players;
	ResponseValue["description"] = Description;
	m_Client->ForgeAugmentServerListPing(ResponseValue);
	if (!Favicon.empty())
	{
		ResponseValue["favicon"] = Printf("data:image/png;base64,%s", Favicon.c_str());
	}

	Json::FastWriter Writer;
	AString Response = Writer.write(ResponseValue);

	cPacketizer Pkt(*this, pktStatusResponse);
	Pkt.WriteString(Response);
}





////////////////////////////////////////////////////////////////////////////////
// cProtocol_1_9_4:

cProtocol_1_9_4::cProtocol_1_9_4(cClientHandle * a_Client, const AString & a_ServerAddress, UInt16 a_ServerPort, UInt32 a_State) :
	Super(a_Client, a_ServerAddress, a_ServerPort, a_State)
{
}





void cProtocol_1_9_4::HandlePacketStatusRequest(cByteBuffer & a_ByteBuffer)
{
	cServer * Server = cRoot::Get()->GetServer();
	AString ServerDescription = Server->GetDescription();
	auto NumPlayers = static_cast<signed>(Server->GetNumPlayers());
	auto MaxPlayers = static_cast<signed>(Server->GetMaxPlayers());
	AString Favicon = Server->GetFaviconData();
	cRoot::Get()->GetPluginManager()->CallHookServerPing(*m_Client, ServerDescription, NumPlayers, MaxPlayers, Favicon);

	// Version:
	Json::Value Version;
	Version["name"] = "Cuberite 1.9.4";
	Version["protocol"] = 110;

	// Players:
	Json::Value Players;
	Players["online"] = NumPlayers;
	Players["max"] = MaxPlayers;
	// TODO: Add "sample"

	// Description:
	Json::Value Description;
	Description["text"] = ServerDescription.c_str();

	// Create the response:
	Json::Value ResponseValue;
	ResponseValue["version"] = Version;
	ResponseValue["players"] = Players;
	ResponseValue["description"] = Description;
	m_Client->ForgeAugmentServerListPing(ResponseValue);
	if (!Favicon.empty())
	{
		ResponseValue["favicon"] = Printf("data:image/png;base64,%s", Favicon.c_str());
	}

	Json::FastWriter Writer;
	AString Response = Writer.write(ResponseValue);

	cPacketizer Pkt(*this, pktStatusResponse);
	Pkt.WriteString(Response);
}





void cProtocol_1_9_4::SendChunkData(int a_ChunkX, int a_ChunkZ, cChunkDataSerializer & a_Serializer)
{
	ASSERT(m_State == 3);  // In game mode?

	// Serialize first, before creating the Packetizer (the packetizer locks a CS)
	// This contains the flags and bitmasks, too
	const AString & ChunkData = a_Serializer.Serialize(cChunkDataSerializer::RELEASE_1_9_4, a_ChunkX, a_ChunkZ);

	cCSLock Lock(m_CSPacket);
	SendData(ChunkData.data(), ChunkData.size());
}





void cProtocol_1_9_4::SendUpdateSign(int a_BlockX, int a_BlockY, int a_BlockZ, const AString & a_Line1, const AString & a_Line2, const AString & a_Line3, const AString & a_Line4)
{
	ASSERT(m_State == 3);  // In game mode?

	// 1.9.4 removed the update sign packet and now uses Update Block Entity
	cPacketizer Pkt(*this, pktUpdateBlockEntity);
	Pkt.WritePosition64(a_BlockX, a_BlockY, a_BlockZ);
	Pkt.WriteBEUInt8(9);  // Action 9 - update sign

	cFastNBTWriter Writer;
	Writer.AddInt("x",        a_BlockX);
	Writer.AddInt("y",        a_BlockY);
	Writer.AddInt("z",        a_BlockZ);
	Writer.AddString("id", "Sign");

	Json::StyledWriter JsonWriter;
	Json::Value Line1;
	Line1["text"] = a_Line1;
	Writer.AddString("Text1", JsonWriter.write(Line1));
	Json::Value Line2;
	Line2["text"] = a_Line2;
	Writer.AddString("Text2", JsonWriter.write(Line2));
	Json::Value Line3;
	Line3["text"] = a_Line3;
	Writer.AddString("Text3", JsonWriter.write(Line3));
	Json::Value Line4;
	Line4["text"] = a_Line4;
	Writer.AddString("Text4", JsonWriter.write(Line4));

	Writer.Finish();
	Pkt.WriteBuf(Writer.GetResult().data(), Writer.GetResult().size());
}





UInt32 cProtocol_1_9_4::GetPacketID(cProtocol::ePacketType a_Packet)
{
	switch (a_Packet)
	{
		case pktCollectEntity:    return 0x48;
		case pktEntityEffect:     return 0x4b;
		case pktEntityProperties: return 0x4a;
		case pktPlayerMaxSpeed:   return 0x4a;
		case pktTeleportEntity:   return 0x49;

		default: return Super::GetPacketID(a_Packet);
	}
}
