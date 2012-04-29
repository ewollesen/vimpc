/*
   Vimpc
   Copyright (C) 2010 - 2012 Nathan Sweetman

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   mpdclient.cpp - provides interaction with the music player daemon
   */

#include "mpdclient.hpp"

#include "assert.hpp"
#include "screen.hpp"
#include "settings.hpp"
#include "vimpc.hpp"

#include "buffer/playlist.hpp"
#include "mode/mode.hpp"
#include "window/error.hpp"

#include <mpd/tag.h>
#include <mpd/status.h>
#include <sys/time.h>

using namespace Mpc;

#define MPDCOMMAND


// Helper functions
uint32_t Mpc::SecondsToMinutes(uint32_t duration)
{
   return static_cast<uint32_t>(duration / 60);
}

uint32_t Mpc::RemainingSeconds(uint32_t duration)
{
   return (duration - (SecondsToMinutes(duration) * 60));
}


CommandList::CommandList(Mpc::Client & client, bool condition) :
   condition_(condition),
   client_   (client)
{
   if (condition_)
   {
      client_.ClearCommand();
      client_.StartCommandList();
   }
}

CommandList::~CommandList()
{
   if (condition_)
   {
      client_.SendCommandList();
   }
}


// Mpc::Client Implementation
Client::Client(Main::Vimpc * vimpc, Main::Settings & settings, Ui::Screen & screen) :
   vimpc_                (vimpc),
   settings_             (settings),
   connection_           (NULL),

   hostname_             (""),
   port_                 (0),
   versionMajor_         (-1),
   versionMinor_         (-1),
   versionPatch_         (-1),
   timeSinceUpdate_      (0),
   timeSinceSong_        (0),
   retried_              (false),

   volume_               (100),
   random_               (false),
   repeat_               (false),
   single_               (false),
   consume_              (false),
   crossfade_            (false),
   crossfadeTime_        (0),
   elapsed_              (0),
   state_                (MPD_STATE_STOP),
   mpdstate_             (MPD_STATE_UNKNOWN),

   currentSong_          (NULL),
   currentStatus_        (NULL),
   currentSongId_        (-1),
   currentSongURI_       (""),
   currentState_         ("Disconnected"),

   screen_               (screen),
   queueVersion_         (-1),
   forceUpdate_          (true),
   listMode_             (false),
   idleMode_             (false)
{
}

Client::~Client()
{
   if (currentStatus_ != NULL)
   {
      mpd_status_free(currentStatus_);
      currentStatus_ = NULL;
   }

   if (currentSong_ != NULL)
   {
      mpd_song_free(currentSong_);
      currentSong_ = NULL;
   }

   DeleteConnection();
}


void Client::Connect(std::string const & hostname, uint16_t port)
{
   std::string connect_hostname = hostname;
   uint16_t    connect_port     = port;
   std::string connect_password = "";
   size_t      pos;

   DeleteConnection();

   if (connect_hostname.empty() == true)
   {
      char * const host_env = getenv("MPD_HOST");

      if (host_env != NULL)
      {
         connect_hostname = host_env;

         pos = connect_hostname.find_last_of("@");
         if ( pos != connect_hostname.npos )
         {
            connect_password = connect_hostname.substr(0, pos);
            connect_hostname = connect_hostname.substr(pos + 1);
         }
      }
      else
      {
         connect_hostname = "localhost";
      }
   }

   if (port == 0)
   {
      char * const port_env = getenv("MPD_PORT");

      if (port_env != NULL)
      {
         connect_port = atoi(port_env);
      }
      else
      {
         connect_port = 0;
      }
   }

   // Connecting may take a long time as this is a single threaded application
   // and the mpd connect is a blocking call, so be sure to update the screen
   // first to let the user know that something is happening
   currentState_ = "Connecting";
   vimpc_->CurrentMode().Refresh();

   hostname_ = connect_hostname;
   port_     = connect_port;

   //! \TODO make the connection async
   connection_ = mpd_connection_new(connect_hostname.c_str(), connect_port, 0);

   CheckError();

   if (Connected() == true)
   {
      retried_ = false;
      screen_.Update();
      DisplaySongInformation();
      vimpc_->OnConnected();

      GetVersion();
      UpdateStatus();

      // Must redraw the library first
      screen_.InvalidateAll();
      screen_.Redraw(Ui::Screen::Library);
      screen_.Redraw(Ui::Screen::Playlist);

      if ((screen_.GetActiveWindow() != Ui::Screen::Library) &&
          (screen_.GetActiveWindow() != Ui::Screen::Playlist))
      {
         screen_.Redraw(screen_.GetActiveWindow());
      }

      UpdateStatus();

      if (connect_password != "")
      {
         Password(connect_password);
      }
   }
}

void Client::Disconnect()
{
   if (Connected() == true)
   {
      DeleteConnection();
   }
}

void Client::Reconnect()
{
   Disconnect();
   Connect(hostname_, port_);
}

void Client::Password(std::string const & password)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_password(connection_, password.c_str());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

std::string Client::Hostname()
{
   return hostname_;
}

uint16_t Client::Port()
{
   return port_;
}

bool Client::Connected() const
{
   return (connection_ != NULL);
}


void Client::Play(uint32_t const playId)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_play_pos(connection_, playId);

      currentSongId_ = playId;
      state_         = MPD_STATE_PLAY;

      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Pause()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_toggle_pause(connection_);

      if (state_ == MPD_STATE_PLAY)
      {
         state_ = MPD_STATE_PAUSE;
      }
      else if (state_ == MPD_STATE_PAUSE)
      {
         state_ = MPD_STATE_PLAY;
      }
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Stop()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_stop(connection_);

      state_ = MPD_STATE_STOP;
      currentSong_ = NULL;
      currentSongId_  = -1;
      currentSongURI_ = "";
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Next()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_next(connection_);
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Previous()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_previous(connection_);
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Seek(int32_t Offset)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_seek_pos(connection_, currentSongId_, elapsed_ + Offset);
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::SeekTo(uint32_t Time)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_seek_pos(connection_, currentSongId_, Time);
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


bool Client::Random()
{
   return random_;
}

void Client::SetRandom(bool const random)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_random(connection_, random);
      random_ = random;
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


bool Client::Single()
{
   return single_;
}

void Client::SetSingle(bool const single)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_single(connection_, single);
      single_ = single;
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


bool Client::Consume()
{
   return consume_;
}

void Client::SetConsume(bool const consume)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_consume(connection_, consume);
      consume_ = consume;
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

bool Client::Repeat()
{
   return repeat_;
}

void Client::SetRepeat(bool const repeat)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_repeat(connection_, repeat);
      repeat_ = repeat;
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

int32_t Client::Crossfade()
{
   if (crossfade_ == true)
   {
      return crossfadeTime_;
   }

   return 0;
}

void Client::SetCrossfade(bool crossfade)
{
   if (crossfade == true)
   {
      SetCrossfade(crossfadeTime_);
   }
   else
   {
      SetCrossfade(static_cast<uint32_t>(0));
   }
}

void Client::SetCrossfade(uint32_t crossfade)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_crossfade(connection_, crossfade);
      crossfade_     = (crossfade != 0);

      if (crossfade_ == true)
      {
         crossfadeTime_ = crossfade;
      }
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

int32_t Client::Volume()
{
   return volume_;
}

void Client::SetVolume(uint32_t volume)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_set_volume(connection_, volume);
      volume_ = volume;
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


void Client::Shuffle()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_shuffle(connection_);
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Move(uint32_t position1, uint32_t position2)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_move(connection_, position1, position2);
      UpdateStatus(true);
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Swap(uint32_t position1, uint32_t position2)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_swap(connection_, position1, position2);
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


void Client::CreatePlaylist(std::string const & name)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_run_save(connection_, name.c_str());
      mpd_run_playlist_clear(connection_, name.c_str());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::SavePlaylist(std::string const & name)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_run_save(connection_, name.c_str());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::LoadPlaylist(std::string const & name)
{
   if (Connected() == true)
   {
      Clear();
      ClearCommand();
      mpd_run_load(connection_, name.c_str());
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::RemovePlaylist(std::string const & name)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_run_rm(connection_, name.c_str());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::AddToNamedPlaylist(std::string const & name, Mpc::Song * song)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_playlist_add(connection_, name.c_str(), song->URI().c_str());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


void Client::SetOutput(Mpc::Output * output, bool enable)
{
   if (enable == true)
   {
      EnableOutput(output);
   }
   else
   {
      DisableOutput(output);
   }
}

void Client::EnableOutput(Mpc::Output * output)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_run_enable_output(connection_, output->Id());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::DisableOutput(Mpc::Output * output)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_run_disable_output(connection_, output->Id());
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


void Client::Add(Mpc::Song * song)
{
   if ((Connected() == true) && (song != NULL))
   {
      (void) Add(*song);
   }
   else if (Connected() == false)
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

uint32_t Client::Add(Mpc::Song & song)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_add(connection_, song.URI().c_str());
      UpdateStatus(true);
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }

   return TotalNumberOfSongs() - 1;
}

uint32_t Client::Add(Mpc::Song & song, uint32_t position)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_add_id_to(connection_, song.URI().c_str(), position);

      if ((currentSongId_ > -1) && (position <= static_cast<uint32_t>(currentSongId_)))
      {
         ++currentSongId_;
      }

      UpdateStatus(true);
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }

   return TotalNumberOfSongs() - 1;
}

uint32_t Client::AddAllSongs()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_add(connection_, "/");
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }

   return TotalNumberOfSongs() - 1;
}

uint32_t Client::Add(std::string const & URI)
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_add(connection_, URI.c_str());
      UpdateStatus();
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }

   return TotalNumberOfSongs() - 1;
}


void Client::Delete(uint32_t position)
{
   if ((Connected() == true) && (TotalNumberOfSongs() > 0))
   {
      ClearCommand();
      mpd_send_delete(connection_, position);

      if ((currentSongId_ > -1) && (position < static_cast<uint32_t>(currentSongId_)))
      {
         --currentSongId_;
      }

      UpdateStatus(true);
   }
   else if (Connected() == false)
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Delete(uint32_t position1, uint32_t position2)
{
   if ((Connected() == true) && (TotalNumberOfSongs() > 0))
   {
      // Only use range if MPD is >= 0.16
      if (versionMinor_ < 16)
      {
         CommandList list(*this);

         for (uint32_t i = 0; i < (position2 - position1); ++i)
         {
            Delete(position1);
         }
      }
      else
      {
         ClearCommand();
         mpd_send_delete_range(connection_, position1, position2);

         if (currentSongId_ > -1)
         {
            uint32_t const songId = static_cast<uint32_t>(currentSongId_);

            if ((position1 < songId) && (position2 < songId))
            {
               currentSongId_ -= position2 - position1;
            }
            else if ((position1 <= songId) && (position2 >= songId))
            {
               currentSongId_ -= (currentSongId_ - position1);
            }
         }
      }

      UpdateStatus(true);
   }
   else if (Connected() == false)
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Clear()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_clear(connection_);
      UpdateStatus(true);
   }
   else if (Connected() == false)
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}


void Client::SearchAny(std::string const & search, bool exact)
{
   if (Connected() == true)
   {
      mpd_search_db_songs(connection_, exact);
      mpd_search_add_any_tag_constraint(connection_, MPD_OPERATOR_DEFAULT, search.c_str());
   }
}

void Client::SearchArtist(std::string const & search, bool exact)
{
   if (Connected() == true)
   {
      mpd_search_db_songs(connection_, exact);
      mpd_search_add_tag_constraint(connection_, MPD_OPERATOR_DEFAULT, MPD_TAG_ARTIST, search.c_str());
   }
}

void Client::SearchGenre(std::string const & search, bool exact)
{
   if (Connected() == true)
   {
      mpd_search_db_songs(connection_, exact);
      mpd_search_add_tag_constraint(connection_, MPD_OPERATOR_DEFAULT, MPD_TAG_GENRE, search.c_str());
   }
}



void Client::SearchAlbum(std::string const & search, bool exact)
{
   if (Connected() == true)
   {
      mpd_search_db_songs(connection_, exact);
      mpd_search_add_tag_constraint(connection_, MPD_OPERATOR_DEFAULT, MPD_TAG_ALBUM, search.c_str());
   }
}

void Client::SearchSong(std::string const & search, bool exact)
{
   if (Connected() == true)
   {
      mpd_search_db_songs(connection_, exact);
      mpd_search_add_tag_constraint(connection_, MPD_OPERATOR_DEFAULT, MPD_TAG_TITLE, search.c_str());
   }
}


std::string Client::CurrentState()
{
   if (Connected() == true)
   {
      if (currentStatus_ != NULL)
      {
         switch (state_)
         {
            case MPD_STATE_UNKNOWN:
               currentState_ = "Unknown";
               break;
            case MPD_STATE_STOP:
               currentState_ = "Stopped";
               break;
            case MPD_STATE_PLAY:
               currentState_ = "Playing";
               break;
            case MPD_STATE_PAUSE:
               currentState_ = "Paused";
               break;

            default:
               break;
         }
      }
   }

   return currentState_;
}


std::string Client::GetCurrentSongURI()
{
   return currentSongURI_;
}

//! \todo rename to GetCurrentSongPos
int32_t Client::GetCurrentSong()
{
   return currentSongId_;
}

uint32_t Client::TotalNumberOfSongs()
{
   uint32_t songTotal = 0;

   if ((Connected() == true) && (currentStatus_ != NULL))
   {
      songTotal = mpd_status_get_queue_length(currentStatus_);
   }

   return songTotal;
}

bool Client::SongIsInQueue(Mpc::Song const & song) const
{
   return (song.Reference() != 0);
}

void Client::DisplaySongInformation()
{
   if ((Connected() == true) && (CurrentState() != "Stopped"))
   {
      if ((currentSong_ != NULL) && (currentStatus_ != NULL))
      {
         mpd_status * const status   = currentStatus_;
         uint32_t     const duration = mpd_song_get_duration(currentSong_);
         uint32_t     const elapsed  = elapsed_;
         uint32_t     const remain   = (duration > elapsed) ? duration - elapsed : 0;
         char const * const cArtist  = mpd_song_get_tag(currentSong_, MPD_TAG_ARTIST, 0);
         char const * const cTitle   = mpd_song_get_tag(currentSong_, MPD_TAG_TITLE, 0);
         std::string  const artist   = (cArtist == NULL) ? "Unknown" : cArtist;
         std::string  const title    = (cTitle  == NULL) ? "Unknown" : cTitle;

         screen_.SetStatusLine("[%5u] %s - %s", GetCurrentSong() + 1, artist.c_str(), title.c_str());

         if (settings_.Get(Setting::TimeRemaining) == false)
         {
            screen_.MoveSetStatus(screen_.MaxColumns() - 14, "[%2d:%.2d |%2d:%.2d]",
                                  SecondsToMinutes(elapsed),  RemainingSeconds(elapsed),
                                  SecondsToMinutes(duration), RemainingSeconds(duration));
         }
         else
         {
            screen_.MoveSetStatus(screen_.MaxColumns() - 15, "[-%2d:%.2d |%2d:%.2d]",
                                  SecondsToMinutes(remain),  RemainingSeconds(remain),
                                  SecondsToMinutes(duration), RemainingSeconds(duration));
         }
      }
   }
   else
   {
      screen_.SetStatusLine("%s","");
   }
}


void Client::Rescan()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_rescan(connection_, "/");
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::Update()
{
   if (Connected() == true)
   {
      ClearCommand();
      mpd_send_update(connection_, "/");
   }
   else
   {
      ErrorString(ErrorNumber::ClientNoConnection);
   }
}

void Client::IncrementTime(long time)
{
   REQUIRE(time >= 0);

   timeSinceUpdate_ += time;
   timeSinceSong_   += time;

   if (state_ == MPD_STATE_PLAY)
   {
      elapsed_ = mpdelapsed_ + (timeSinceUpdate_ / 1000);
   }

   if ((currentSong_ != NULL) &&
       (elapsed_ > mpd_song_get_duration(currentSong_)))
   {
      elapsed_ = 0;

      if (timeSinceUpdate_ >= 1000)
      {
         UpdateStatus();
      }
   }
}

long Client::TimeSinceUpdate()
{
   return timeSinceUpdate_;
}

void Client::IdleMode()
{
   if ((Connected() == true) && (settings_.Get(Setting::Polling) == false) &&
       (idleMode_ == false))
   {
      idleMode_ = true;
      mpd_send_idle(connection_);
   }
}

bool Client::HadEvents()
{
   if ((Connected() == true) && (settings_.Get(Setting::Polling) == false) &&
       (idleMode_ == true))
   {
      idleMode_ = false;
      return (mpd_run_noidle(connection_) != 0);
   }

   return false;
}

void Client::UpdateCurrentSong()
{
   if ((Connected() == true))
   {
      if (listMode_ == false)
      {
         if (currentSong_ != NULL)
         {
            mpd_song_free(currentSong_);
            currentSong_ = NULL;
            currentSongId_  = -1;
            currentSongURI_ = "";
         }

         if (state_ != MPD_STATE_STOP)
         {
            ClearCommand();
            timeSinceSong_ = 0;
            currentSong_ = mpd_run_current_song(connection_);
            CheckError();

            if (currentSong_ != NULL)
            {
               currentSongId_  = mpd_song_get_pos(currentSong_);
               currentSongURI_ = mpd_song_get_uri(currentSong_);
            }
         }
      }
   }
   else
   {
      currentSongId_ = -1;
      currentSongURI_ = "";
   }
}

void Client::UpdateDisplay()
{
   // Try and correct display without requesting status from mpd
   UpdateCurrentSongPosition();
}


void Client::ClearCommand()
{
   if ((listMode_ == false) && (Connected() == true))
   {
      mpd_response_finish(connection_);
      CheckError();
   }
}

void Client::StartCommandList()
{
   if (Connected() == true)
   {
      listMode_ = true;
      mpd_command_list_begin(connection_, true);
   }
}

void Client::SendCommandList()
{
   if (Connected() == true)
   {
      mpd_command_list_end(connection_);
      mpd_response_finish(connection_);

      CheckError();

      listMode_ = false;
      UpdateStatus(true);
   }
}


unsigned int Client::QueueVersion()
{
   return queueVersion_;
}

void Client::UpdateStatus(bool ExpectUpdate)
{
   ClearCommand();

   if ((Connected() == true) && (listMode_ == false))
   {
      if (currentStatus_ != NULL)
      {
         mpd_status_free(currentStatus_);
         currentStatus_ = NULL;
      }

      timeSinceUpdate_ = 0;
      currentStatus_   = mpd_run_status(connection_);
      CheckError();

      if (currentStatus_ != NULL)
      {
         unsigned int version  = mpd_status_get_queue_version(currentStatus_);
         unsigned int qVersion = static_cast<uint32_t>(queueVersion_);

         volume_   = mpd_status_get_volume(currentStatus_);
         random_   = mpd_status_get_random(currentStatus_);
         repeat_   = mpd_status_get_repeat(currentStatus_);
         single_   = mpd_status_get_single(currentStatus_);
         consume_  = mpd_status_get_consume(currentStatus_);
         crossfade_ = (mpd_status_get_crossfade(currentStatus_) > 0);

         if (crossfade_ == true)
         {
            crossfadeTime_ = mpd_status_get_crossfade(currentStatus_);
         }

         // Check if we need to update the current song
         if ((mpdstate_ != mpd_status_get_state(currentStatus_)) ||
             ((mpdstate_ != MPD_STATE_STOP) && (currentSong_ == NULL)) ||
             ((currentSong_ != NULL) &&
              ((elapsed_ >= mpd_song_get_duration(currentSong_) - 3) ||
               (mpd_status_get_elapsed_time(currentStatus_) < mpdelapsed_) ||
               (mpd_status_get_elapsed_time(currentStatus_) <= 3))))
         {
            UpdateCurrentSong();
         }

         mpdstate_   = mpd_status_get_state(currentStatus_);
         mpdelapsed_ = mpd_status_get_elapsed_time(currentStatus_);
         state_      = mpdstate_;

         if ((queueVersion_ > -1) &&
             ((version > qVersion + 1) || ((version > qVersion) && (ExpectUpdate == false))))
         {
            ForEachQueuedSongChanges(qVersion, Main::Playlist(), static_cast<void (Mpc::Playlist::*)(uint32_t, Mpc::Song *)>(&Mpc::Playlist::Replace));
            Main::Playlist().Crop(TotalNumberOfSongs());
         }

         queueVersion_ = version;
      }
   }
}

void Client::UpdateCurrentSongPosition()
{
   if ((currentSong_ != NULL) && (currentSongId_ >= 0) &&
       (currentSongId_ < Main::Playlist().Size()) &&
       (*Main::Playlist().Get(currentSongId_) != *currentSong_))
   {
      currentSongId_ = -1;

      for (uint32_t i = 0; i < screen_.MaxRows(); ++i)
      {
         int id = i + screen_.ActiveWindow().FirstLine();

         if ((id < Main::Playlist().Size()) &&
             (*Main::Playlist().Get(id) == *currentSong_))
         {
            currentSongId_ = id;
            break;
         }
      }
   }
}


Song * Client::CreateSong(uint32_t id, mpd_song const * const song, bool songInLibrary) const
{
   Song * const newSong = new Song();

   newSong->SetArtist   (mpd_song_get_tag(song, MPD_TAG_ARTIST, 0));
   newSong->SetAlbum    (mpd_song_get_tag(song, MPD_TAG_ALBUM,  0));
   newSong->SetTitle    (mpd_song_get_tag(song, MPD_TAG_TITLE,  0));
   newSong->SetTrack    (mpd_song_get_tag(song, MPD_TAG_TRACK,  0));
   newSong->SetURI      (mpd_song_get_uri(song));
   newSong->SetDuration (mpd_song_get_duration(song));

   return newSong;
}


void Client::GetVersion()
{
   if (Connected() == true)
   {
      unsigned const * version = mpd_connection_get_server_version(connection_);
      CheckError();

      if (version != NULL)
      {
         versionMajor_ = version[0];
         versionMinor_ = version[1];
         versionPatch_ = version[2];
      }
   }
}

void Client::CheckError()
{
   if (connection_ != NULL)
   {
      if (mpd_connection_get_error(connection_) != MPD_ERROR_SUCCESS)
      {
         char error[255];
         snprintf(error, 255, "Client Error: %s",  mpd_connection_get_error_message(connection_));
         Error(ErrorNumber::ClientError, error);

         bool ClearError = mpd_connection_clear_error(connection_);

         if (ClearError == false)
         {
            DeleteConnection();

            if ((settings_.Get(Setting::Reconnect) == true) && (retried_ == false))
            {
               retried_ = true;
               Connect(hostname_, port_);
            }
         }
      }
   }
}

void Client::DeleteConnection()
{
   listMode_     = false;
   currentState_ = "Disconnected";
   volume_       = -1;
   random_       = false;
   single_       = false;
   consume_      = false;
   repeat_       = false;

   versionMajor_ = -1;
   versionMinor_ = -1;
   versionPatch_ = -1;
   queueVersion_ = -1;

   if (connection_ != NULL)
   {
      mpd_connection_free(connection_);
      connection_ = NULL;
   }

   ENSURE(connection_ == NULL);
}


/* vim: set sw=3 ts=3: */
