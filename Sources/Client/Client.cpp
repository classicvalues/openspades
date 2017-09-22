/*
 Copyright (c) 2013 yvt
 based on code of pysnip (c) Mathias Kaerlev 2011-2012.

 This file is part of OpenSpades.

 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.

 */

#include <cstdarg>
#include <cstdlib>
#include <ctime>

#include "Client.h"
#include "Fonts.h"
#include <Core/FileManager.h>
#include <Core/IStream.h>
#include <Core/Settings.h>
#include <Core/Strings.h>

#include "IAudioChunk.h"
#include "IAudioDevice.h"

#include "CenterMessageView.h"
#include "ChatWindow.h"
#include "ClientPlayer.h"
#include "ClientUI.h"
#include "HurtRingView.h"
#include "MapView.h"
#include "PaletteView.h"
#include "ScoreboardView.h"
#include "TCProgressView.h"

#include "Corpse.h"
#include "ILocalEntity.h"
#include "SmokeSpriteEntity.h"

#include "GameMap.h"
#include "GameMapWrapper.h"
#include "Weapon.h"
#include "World.h"

#include "NetClient.h"

DEFINE_SPADES_SETTING(cg_chatBeep, "1");

DEFINE_SPADES_SETTING(cg_serverAlert, "1");

DEFINE_SPADES_SETTING(cg_SkipDeadPlayersWhenDead, "1");

SPADES_SETTING(cg_playerName);

namespace spades {
	namespace client {

		std::random_device r_device_client;
		std::mt19937_64 mt_engine_client(
		  r_device_client()); // Seed Mersenne twister with non-deterministic 32-bit seed

		Client::Client(IRenderer *r, IAudioDevice *audioDev, const ServerAddress &host, float pixelRatio,
		               FontManager *fontManager)
		    : renderer(r),
		      audioDevice(audioDev),
			  pixelRatio(pixelRatio),
		      playerName(cg_playerName.operator std::string().substr(0, 15)),
		      hasDelayedReload(false),
		      hostname(host),
		      logStream(nullptr),
		      fontManager(fontManager),

		      readyToClose(false),
		      scoreboardVisible(false),
		      flashlightOn(false),

		      frameToRendererInit(5),
		      time(0.f),
		      timeSinceInit(0.f),

		      lastAliveTime(0.f),
		      lastKills(0),

		      focalLength(20.f),
		      targetFocalLength(20.f),
		      autoFocusEnabled(true),

		      hitFeedbackIconState(0.f),
		      hitFeedbackFriendly(false),
		      localFireVibrationTime(-1.f),
		      lastPosSentTime(0.f),
		      worldSubFrame(0.f),
		      grenadeVibration(0.f),
		      grenadeVibrationSlow(0.f),
		      lastMyCorpse(nullptr),
		      hasLastTool(false),

		      nextScreenShotIndex(0),
		      nextMapShotIndex(0),

		      alertDisappearTime(-10000.f),

		      // FIXME: preferences?
		      corpseSoftTimeLimit(30.f), // FIXME: this is not used
		      corpseSoftLimit(6),
		      corpseHardLimit(16),

		      followYaw(0.f),
		      followPitch(0.f) {
			SPADES_MARK_FUNCTION();
			SPLog("Initializing...");

			renderer->SetFogDistance(128.f);
			renderer->SetFogColor(MakeVector3(.8f, 1.f, 1.f));

			chatWindow.reset(new ChatWindow(this, GetRenderer(), fontManager->GetGuiFont(), false));
			killfeedWindow.reset(
			  new ChatWindow(this, GetRenderer(), fontManager->GetGuiFont(), true));

			hurtRingView.reset(new HurtRingView(this));
			centerMessageView.reset(new CenterMessageView(this, fontManager->GetLargeFont()));
			mapView.reset(new MapView(this, false));
			largeMapView.reset(new MapView(this, true));
			scoreboard.reset(new ScoreboardView(this));
			paletteView.reset(new PaletteView(this));
			tcView.reset(new TCProgressView(this));
			scriptedUI.Set(new ClientUI(renderer, audioDev, fontManager, pixelRatio, this), false);

			renderer->SetGameMap(nullptr);
		}

		void Client::SetWorld(spades::client::World *w) {
			SPADES_MARK_FUNCTION();

			if (world.get() == w) {
				return;
			}

			scriptedUI->CloseUI();

			RemoveAllCorpses();
			RemoveAllLocalEntities();

			lastHealth = 0;
			lastHurtTime = -100.f;
			hurtRingView->ClearAll();
			scoreboardVisible = false;
			flashlightOn = false;

			for (size_t i = 0; i < clientPlayers.size(); i++) {
				if (clientPlayers[i]) {
					clientPlayers[i]->Invalidate();
				}
			}
			clientPlayers.clear();

			if (world) {
				world->SetListener(nullptr);
				renderer->SetGameMap(nullptr);
				audioDevice->SetGameMap(nullptr);
				world = nullptr;
				map = nullptr;
			}
			world.reset(w);
			if (world) {
				SPLog("World set");

				// initialize player view objects
				clientPlayers.resize(world->GetNumPlayerSlots());
				for (size_t i = 0; i < world->GetNumPlayerSlots(); i++) {
					Player *p = world->GetPlayer(static_cast<unsigned int>(i));
					if (p) {
						clientPlayers[i] = new ClientPlayer(p, this);
					} else {
						clientPlayers[i] = nullptr;
					}
				}

				world->SetListener(this);
				map = world->GetMap();
				renderer->SetGameMap(map);
				audioDevice->SetGameMap(map);
				NetLog("------ World Loaded ------");
			} else {

				SPLog("World removed");
				NetLog("------ World Unloaded ------");
			}

			worldSubFrame = 0.f;
			worldSetTime = time;

			if (!world) {
				scriptedUI->LeaveLimboWindow();
			}
		}

		Client::~Client() {
			SPADES_MARK_FUNCTION();

			NetLog("Disconnecting");

			DrawDisconnectScreen();

			if (logStream) {
				SPLog("Closing netlog");
				logStream.reset();
			}

			if (net) {
				SPLog("Disconnecting");
				net->Disconnect();
				net.reset();
			}

			SPLog("Disconnected");

			RemoveAllLocalEntities();
			RemoveAllCorpses();

			renderer->SetGameMap(nullptr);
			audioDevice->SetGameMap(nullptr);

			for (size_t i = 0; i < clientPlayers.size(); i++) {
				if (clientPlayers[i]) {
					clientPlayers[i]->Invalidate();
				}
			}
			clientPlayers.clear();

			scriptedUI->ClientDestroyed();
			tcView.reset();
			scoreboard.reset();
			mapView.reset();
			largeMapView.reset();
			chatWindow.reset();
			killfeedWindow.reset();
			paletteView.reset();
			centerMessageView.reset();
			hurtRingView.reset();
			world.reset();
		}

		/** Initiate an initialization which likely to take some time */
		void Client::DoInit() {
			renderer->Init();
			SmokeSpriteEntity::Preload(renderer);

			renderer->RegisterImage("Textures/Fluid.png");
			renderer->RegisterImage("Textures/WaterExpl.png");
			renderer->RegisterImage("Gfx/White.tga");
			audioDevice->RegisterSound("Sounds/Weapons/Block/Build.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Impacts/FleshLocal1.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Impacts/FleshLocal2.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Impacts/FleshLocal3.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Impacts/FleshLocal4.opus");
			audioDevice->RegisterSound("Sounds/Misc/SwitchMapZoom.opus");
			audioDevice->RegisterSound("Sounds/Misc/OpenMap.opus");
			audioDevice->RegisterSound("Sounds/Misc/CloseMap.opus");
			audioDevice->RegisterSound("Sounds/Player/Flashlight.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep1.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep2.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep3.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep4.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep5.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep6.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep7.opus");
			audioDevice->RegisterSound("Sounds/Player/Footstep8.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade1.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade2.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade3.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade4.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade5.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade6.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade7.opus");
			audioDevice->RegisterSound("Sounds/Player/Wade8.opus");
			audioDevice->RegisterSound("Sounds/Player/Run1.opus");
			audioDevice->RegisterSound("Sounds/Player/Run2.opus");
			audioDevice->RegisterSound("Sounds/Player/Run3.opus");
			audioDevice->RegisterSound("Sounds/Player/Run4.opus");
			audioDevice->RegisterSound("Sounds/Player/Run5.opus");
			audioDevice->RegisterSound("Sounds/Player/Run6.opus");
			audioDevice->RegisterSound("Sounds/Player/Run7.opus");
			audioDevice->RegisterSound("Sounds/Player/Run8.opus");
			audioDevice->RegisterSound("Sounds/Player/Run9.opus");
			audioDevice->RegisterSound("Sounds/Player/Run10.opus");
			audioDevice->RegisterSound("Sounds/Player/Run11.opus");
			audioDevice->RegisterSound("Sounds/Player/Run12.opus");
			audioDevice->RegisterSound("Sounds/Player/Jump.opus");
			audioDevice->RegisterSound("Sounds/Player/Land.opus");
			audioDevice->RegisterSound("Sounds/Player/WaterJump.opus");
			audioDevice->RegisterSound("Sounds/Player/WaterLand.opus");
			audioDevice->RegisterSound("Sounds/Weapons/SwitchLocal.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Switch.opus");
			audioDevice->RegisterSound("Sounds/Weapons/Restock.opus");
			audioDevice->RegisterSound("Sounds/Weapons/RestockLocal.opus");
			audioDevice->RegisterSound("Sounds/Weapons/AimDownSightLocal.opus");
			renderer->RegisterImage("Gfx/Ball.png");
			renderer->RegisterModel("Models/Player/Dead.kv6");
			renderer->RegisterImage("Gfx/Spotlight.png");
			renderer->RegisterImage("Gfx/Glare.png");
			renderer->RegisterModel("Models/Weapons/Spade/Spade.kv6");
			renderer->RegisterModel("Models/Weapons/Block/Block2.kv6");
			renderer->RegisterModel("Models/Weapons/Grenade/Grenade.kv6");
			renderer->RegisterModel("Models/Weapons/SMG/Weapon.kv6");
			renderer->RegisterModel("Models/Weapons/SMG/WeaponNoMagazine.kv6");
			renderer->RegisterModel("Models/Weapons/SMG/Magazine.kv6");
			renderer->RegisterModel("Models/Weapons/Rifle/Weapon.kv6");
			renderer->RegisterModel("Models/Weapons/Rifle/WeaponNoMagazine.kv6");
			renderer->RegisterModel("Models/Weapons/Rifle/Magazine.kv6");
			renderer->RegisterModel("Models/Weapons/Shotgun/Weapon.kv6");
			renderer->RegisterModel("Models/Weapons/Shotgun/WeaponNoPump.kv6");
			renderer->RegisterModel("Models/Weapons/Shotgun/Pump.kv6");
			renderer->RegisterModel("Models/Player/Arm.kv6");
			renderer->RegisterModel("Models/Player/UpperArm.kv6");
			renderer->RegisterModel("Models/Player/LegCrouch.kv6");
			renderer->RegisterModel("Models/Player/TorsoCrouch.kv6");
			renderer->RegisterModel("Models/Player/Leg.kv6");
			renderer->RegisterModel("Models/Player/Torso.kv6");
			renderer->RegisterModel("Models/Player/Arms.kv6");
			renderer->RegisterModel("Models/Player/Head.kv6");
			renderer->RegisterModel("Models/MapObjects/Intel.kv6");
			renderer->RegisterModel("Models/MapObjects/CheckPoint.kv6");
			renderer->RegisterImage("Gfx/Bullet/7.62mm.png");
			renderer->RegisterImage("Gfx/Bullet/9mm.png");
			renderer->RegisterImage("Gfx/Bullet/12gauge.png");
			renderer->RegisterImage("Gfx/CircleGradient.png");
			renderer->RegisterImage("Gfx/HurtSprite.png");
			renderer->RegisterImage("Gfx/HurtRing2.png");
			audioDevice->RegisterSound("Sounds/Feedback/Chat.opus");

			if (mumbleLink.init())
				SPLog("Mumble linked");
			else
				SPLog("Mumble link failed");

			mumbleLink.setContext(hostname.ToString(false));
			mumbleLink.setIdentity(playerName);

			SPLog("Started connecting to '%s'", hostname.ToString(true).c_str());
			net.reset(new NetClient(this));
			net->Connect(hostname);

			// decide log file name
			std::string fn = hostname.ToString(false);
			std::string fn2;
			{
				time_t t;
				struct tm tm;
				::time(&t);
				tm = *localtime(&t);
				char buf[256];
				sprintf(buf, "%04d%02d%02d%02d%02d%02d_", tm.tm_year + 1900, tm.tm_mon + 1,
				        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
				fn2 = buf;
			}
			for (size_t i = 0; i < fn.size(); i++) {
				char c = fn[i];
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
					fn2 += c;
				} else {
					fn2 += '_';
				}
			}
			fn2 = "NetLogs/" + fn2 + ".log";

			try {
				logStream.reset(FileManager::OpenForWriting(fn2.c_str()));
				SPLog("Netlog Started at '%s'", fn2.c_str());
			} catch (const std::exception &ex) {
				SPLog("Failed to open netlog file '%s' (%s)", fn2.c_str(), ex.what());
			}
		}

		void Client::RunFrame(float dt) {
			SPADES_MARK_FUNCTION();

			fpsCounter.MarkFrame();

			if (frameToRendererInit > 0) {
				// waiting for renderer initialization

				DrawStartupScreen();

				frameToRendererInit--;
				if (frameToRendererInit == 0) {
					DoInit();

				} else {
					return;
				}
			}

			timeSinceInit += std::min(dt, .03f);

			// update network
			try {
				if (net->GetStatus() == NetClientStatusConnected)
					net->DoEvents(0);
				else
					net->DoEvents(10);
			} catch (const std::exception &ex) {
				if (net->GetStatus() == NetClientStatusNotConnected) {
					SPLog("Disconnected because of error:\n%s", ex.what());
					NetLog("Disconnected because of error:\n%s", ex.what());
					throw;
				} else {
					SPLog("Exception while processing network packets (ignored):\n%s", ex.what());
				}
			}

			hurtRingView->Update(dt);
			centerMessageView->Update(dt);
			mapView->Update(dt);
			largeMapView->Update(dt);

			UpdateAutoFocus(dt);

			if (world) {
				UpdateWorld(dt);
				mumbleLink.update(world->GetLocalPlayer());
			} else {
				renderer->SetFogColor(MakeVector3(0.f, 0.f, 0.f));
			}

			chatWindow->Update(dt);
			killfeedWindow->Update(dt);

			// CreateSceneDefinition also can be used for sounds
			SceneDefinition sceneDef = CreateSceneDefinition();
			lastSceneDef = sceneDef;

			// Update sounds
			try {
				audioDevice->Respatialize(sceneDef.viewOrigin, sceneDef.viewAxis[2],
				                          sceneDef.viewAxis[1]);
			} catch (const std::exception &ex) {
				SPLog("Audio subsystem returned error (ignored):\n%s", ex.what());
			}

			// render scene
			DrawScene();

			// draw 2d
			Draw2D();

			// draw scripted GUI
			scriptedUI->RunFrame(dt);
			if (scriptedUI->WantsClientToBeClosed())
				readyToClose = true;

			// Well done!
			renderer->FrameDone();
			renderer->Flip();

			// reset all "delayed actions" (in case we forget to reset these)
			hasDelayedReload = false;

			time += dt;
		}

		void Client::Spawn(int teamId, int weaponId) {
			WeaponType weap = static_cast<WeaponType>(weaponId);
			int team = teamId;
			if (team == 2)
				team = 255;

			if (!world->GetLocalPlayer() || world->GetLocalPlayer()->GetTeamId() >= 2) {
				// join
				if (team == 255) {
					// weaponId doesn't matter for spectators, but
					// NetClient doesn't like invalid weapon ID
					weap = WeaponType::RIFLE_WEAPON;
				}
				net->SendJoin(team, weap, playerName, lastKills);
			} else {
				Player *p = world->GetLocalPlayer();
				if (p->GetTeamId() != team) {
					net->SendTeamChange(team);
				}
				if (team != 255 && p->GetWeapon()->GetWeaponType() != weap) {
					net->SendWeaponChange(weap);
				}
			}
		}

		void Client::ShowAlert(const std::string &contents, AlertType type) {
			float timeout;
			switch (type) {
				case AlertType::Notice: timeout = 2.5f; break;
				case AlertType::Warning: timeout = 3.f; break;
				case AlertType::Error: timeout = 3.f; break;
			}
			ShowAlert(contents, type, timeout);
		}

		void Client::ShowAlert(const std::string &contents, AlertType type, float timeout,
		                       bool quiet) {
			alertType = type;
			alertContents = contents;
			alertDisappearTime = time + timeout;
			alertAppearTime = time;

			if (type != AlertType::Notice && !quiet) {
				PlayAlertSound();
			}
		}

		void Client::PlayAlertSound() {
			Handle<IAudioChunk> chunk = audioDevice->RegisterSound("Sounds/Feedback/Alert.opus");
			audioDevice->PlayLocal(chunk, AudioParam());
		}

		/** Records chat message/game events to the log file. */
		void Client::NetLog(const char *format, ...) {
			char buf[4096];
			va_list va;
			va_start(va, format);
			vsnprintf(buf, sizeof(buf), format, va);
			va_end(va);
			std::string str = buf;

			time_t t;
			struct tm tm;
			::time(&t);
			tm = *localtime(&t);

			std::string timeStr = asctime(&tm);

			// remove '\n' in the end of the result of asctime().
			timeStr.resize(timeStr.size() - 1);

			snprintf(buf, sizeof(buf), "%s %s\n", timeStr.c_str(), str.c_str());
			buf[sizeof(buf) - 1] = 0;

			std::string outStr = EscapeControlCharacters(buf);

			printf("%s", outStr.c_str());

			if (logStream) {
				logStream->Write(outStr);
				logStream->Flush();
			}
		}

#pragma mark - Snapshots

		void Client::TakeMapShot() {

			try {
				std::string name = MapShotPath();
				{
					std::unique_ptr<IStream> stream(FileManager::OpenForWriting(name.c_str()));
					try {
						GameMap *map = GetWorld()->GetMap();
						if (map == nullptr) {
							SPRaise("No map loaded");
						}
						map->Save(stream.get());
					} catch (...) {
						throw;
					}
				}

				std::string msg;
				msg = _Tr("Client", "Map saved: {0}", name);
				ShowAlert(msg, AlertType::Notice);
			} catch (const Exception &ex) {
				std::string msg;
				msg = _Tr("Client", "Saving map failed: ");
				msg += ex.GetShortMessage();
				ShowAlert(msg, AlertType::Error);
				SPLog("Saving map failed: %s", ex.what());
			} catch (const std::exception &ex) {
				std::string msg;
				msg = _Tr("Client", "Saving map failed: ");
				msg += ex.what();
				ShowAlert(msg, AlertType::Error);
				SPLog("Saving map failed: %s", ex.what());
			}
		}

		std::string Client::MapShotPath() {
			char buf[256];
			for (int i = 0; i < 10000; i++) {
				sprintf(buf, "Mapshots/shot%04d.vxl", nextScreenShotIndex);
				if (FileManager::FileExists(buf)) {
					nextScreenShotIndex++;
					if (nextScreenShotIndex >= 10000)
						nextScreenShotIndex = 0;
					continue;
				}

				return buf;
			}

			SPRaise("No free file name");
		}

#pragma mark - Chat Messages

		void Client::PlayerSentChatMessage(spades::client::Player *p, bool global,
		                                   const std::string &msg) {
			{
				std::string s;
				if (global)
					//! Prefix added to global chat messages.
					//!
					//! Example: [Global] playername (Red) blah blah
					//!
					//! Crowdin warns that this string shouldn't be translated,
					//! but it actually can be.
					//! The extra whitespace is not a typo.
					s = _Tr("Client", "[Global] ");
				s += ChatWindow::TeamColorMessage(p->GetName(), p->GetTeamId());
				s += ": ";
				s += msg;
				chatWindow->AddMessage(s);
			}
			{
				std::string s;
				if (global)
					s = "[Global] ";
				s += p->GetName();
				s += ": ";
				s += msg;

				auto col = p->GetTeamId() < 2 ? world->GetTeam(p->GetTeamId()).color
				                              : IntVector3::Make(255, 255, 255);

				scriptedUI->RecordChatLog(
				  s, MakeVector4(col.x / 255.f, col.y / 255.f, col.z / 255.f, 0.8f));
			}
			if (global)
				NetLog("[Global] %s (%s): %s", p->GetName().c_str(),
				       world->GetTeam(p->GetTeamId()).name.c_str(), msg.c_str());
			else
				NetLog("[Team] %s (%s): %s", p->GetName().c_str(),
				       world->GetTeam(p->GetTeamId()).name.c_str(), msg.c_str());

			if ((!IsMuted()) && (int)cg_chatBeep) {
				Handle<IAudioChunk> chunk = audioDevice->RegisterSound("Sounds/Feedback/Chat.opus");
				audioDevice->PlayLocal(chunk, AudioParam());
			}
		}

		void Client::ServerSentMessage(const std::string &msg) {
			NetLog("%s", msg.c_str());
			scriptedUI->RecordChatLog(msg, Vector4::Make(1.f, 1.f, 1.f, 0.8f));

			if (cg_serverAlert) {
				if (msg.substr(0, 3) == "N% ") {
					ShowAlert(msg.substr(3), AlertType::Notice);
					return;
				}
				if (msg.substr(0, 3) == "!% ") {
					ShowAlert(msg.substr(3), AlertType::Error);
					return;
				}
				if (msg.substr(0, 3) == "%% ") {
					ShowAlert(msg.substr(3), AlertType::Warning);
					return;
				}
				if (msg.substr(0, 3) == "C% ") {
					centerMessageView->AddMessage(msg.substr(3));
					return;
				}
			}

			chatWindow->AddMessage(msg);
		}

#pragma mark - Follow / Spectate

		void Client::FollowNextPlayer(bool reverse) {
			int myTeam = 2;
			if (world->GetLocalPlayer()) {
				myTeam = world->GetLocalPlayer()->GetTeamId();
			}

			int nextId = followingPlayerId;
			do {
				reverse ? --nextId : ++nextId;
				if (nextId >= static_cast<int>(world->GetNumPlayerSlots()))
					nextId = 0;
				if (nextId < 0)
					nextId = static_cast<int>(world->GetNumPlayerSlots() - 1);

				Player *p = world->GetPlayer(nextId);
				if (p == nullptr)
					continue;
				if (myTeam < 2 && p->GetTeamId() != myTeam)
					continue;

				if (myTeam < 2 && cg_SkipDeadPlayersWhenDead && !p->IsAlive())
					// Skip dead players when not spectator
					continue;
				if (p->GetFront().GetPoweredLength() < .01f)
					continue;
				if (p->GetTeamId() >= 2){
					continue; // Don't spectate spectators
				}

				break;
			} while (nextId != followingPlayerId);

			followingPlayerId = nextId;
		}

		bool Client::IsFollowing() {
			if (!world)
				return false;
			if (!world->GetLocalPlayer())
				return false;
			Player *p = world->GetLocalPlayer();
			if (p->GetTeamId() >= 2)
				return true;
			if (p->IsAlive())
				return false;
			else
				return true;
		}
	}
}
