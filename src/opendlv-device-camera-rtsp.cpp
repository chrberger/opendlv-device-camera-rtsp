/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2020, Live Networks, Inc.  All rights reserved
// A demo application, showing how to create and run a RTSP client (that can potentially receive multiple streams concurrently).
//
// NOTE: This code - although it builds a running application - is intended only to illustrate how to develop your own RTSP
// client application.  For a full-featured RTSP client application - with much more functionality, and many options - see
// "openRTSP": http://www.live555.com/openRTSP/

#include <memory>
#include <mutex>

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "H264VideoRTPSource.hh"

#include "sps_decoder.hpp"
#include "cluon-complete.hpp"
#include "opendlv-standard-message-set.hpp"

// Forward function definitions:

// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void* clientData, char const* reason);
  // called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void* clientData);
  // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// The main streaming routine (for each "rtsp://" URL):
void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, bool TCP, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile);

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

char eventLoopWatchVariable = 0;
void sig_handler(int signo)
{
    if (signo == SIGINT) {
        printf("received SIGINT\n");
        eventLoopWatchVariable = 1;
    }
}


int main(int argc, char** argv) {
    int32_t retCode{1};
    auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
    if ( (0 == commandlineArguments.count("url")) ||
         (0 == commandlineArguments.count("cid")) ) {
      std::cerr << argv[0] << " interfaces with the given RTSP-based camera and sends the compressed frames as OpenDLV messages." << std::endl
        << "Usage:   " << argv[0] << " --url=<URL> [--transport=<nr>] --cid=<CID> [--id=<ID>] [--verbose]" << std::endl
        << "         --cid:       CID of the OD4Session to receive Envelopes for recording" << std::endl
        << "         --id:        ID to use in case of multiple instances of running microservices." << std::endl
        << "         --url:       URL providing an RTSP stream" << std::endl
        << "         --transport: 0: RTP unicast (default), 1: RTP over TCP" << std::endl
        << "         --verbose:   show further information" << std::endl
        << "         --remote:    enable remotely activated recording" << std::endl
        << "         --rec:       name of the recording file; default: YYYY-MM-DD_HHMMSS.rec" << std::endl
        << "         --recsuffix: additional suffix to add to the .rec file" << std::endl
        << "Example: " << argv[0] << " --url=rtsp://10.42.45.50/axis-media/media.amp?camera=1 --cid=111 --remote" << std::endl;
    } else {
        const std::string URL{commandlineArguments["url"]};
        const uint32_t ID{(commandlineArguments.count("id") != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["id"])) : 0};
        const bool TRANSPORT_OVER_TCP{static_cast<bool>((commandlineArguments.count("transport") != 0) ? static_cast<bool>(std::stoi(commandlineArguments["transport"]) == 1) : 0)};
        const bool VERBOSE{commandlineArguments.count("verbose") != 0};

        auto getYYYYMMDD_HHMMSS = [](){
          cluon::data::TimeStamp now = cluon::time::now();

          long int _seconds = now.seconds();
          std::time_t _secondsTemp = static_cast<std::time_t>(_seconds);
          struct tm *tm = localtime(&_secondsTemp);

          uint32_t year = (1900 + tm->tm_year);
          uint32_t month = (1 + tm->tm_mon);
          uint32_t dayOfMonth = tm->tm_mday;
          uint32_t hours = tm->tm_hour;
          uint32_t minutes = tm->tm_min;
          uint32_t seconds = tm->tm_sec;

          std::stringstream sstr;
          sstr << year << "-" << ( (month < 10) ? "0" : "" ) << month << "-" << ( (dayOfMonth < 10) ? "0" : "" ) << dayOfMonth
                         << "_" << ( (hours < 10) ? "0" : "" ) << hours
                         << ( (minutes < 10) ? "0" : "" ) << minutes
                         << ( (seconds < 10) ? "0" : "" ) << seconds;

          std::string retVal{sstr.str()};
          return retVal;
        };
        const bool REMOTE{commandlineArguments.count("remote") != 0};
        const std::string REC{(commandlineArguments["rec"].size() != 0) ? commandlineArguments["rec"] : ""};
        const std::string RECSUFFIX{commandlineArguments["recsuffix"]};
        const std::string NAME_RECFILE{(REC.size() != 0) ? REC + RECSUFFIX : (getYYYYMMDD_HHMMSS() + RECSUFFIX + ".rec")};

        std::unique_ptr<cluon::OD4Session> od4{new cluon::OD4Session(static_cast<uint16_t>(std::stoi(commandlineArguments["cid"])))};
        std::string nameOfRecFile;
        std::mutex recFileMutex{};
        std::unique_ptr<std::fstream> recFile{nullptr};
        if (!REMOTE) {
          recFile.reset(new std::fstream(NAME_RECFILE.c_str(), std::ios::out|std::ios::binary|std::ios::trunc));
          std::cout << "[opendlv-video-camera-rtsp]: Created " << NAME_RECFILE << "." << std::endl;
        }
        else {
          od4.reset(new cluon::OD4Session(static_cast<uint16_t>(std::stoi(commandlineArguments["cid"])),
              [REC, RECSUFFIX, getYYYYMMDD_HHMMSS, &recFileMutex, &recFile, &nameOfRecFile](cluon::data::Envelope &&envelope) noexcept {
            if (cluon::data::RecorderCommand::ID() == envelope.dataType()) {
              std::lock_guard<std::mutex> lck(recFileMutex);
              cluon::data::RecorderCommand rc = cluon::extractMessage<cluon::data::RecorderCommand>(std::move(envelope));
              if (1 == rc.command()) {
                if (recFile && recFile->good()) {
                  recFile->flush();
                  recFile->close();
                  recFile = nullptr;
                  std::cout << "[opendlv-video-camera-rtsp]: Closed " << nameOfRecFile << "." << std::endl;
                }
                nameOfRecFile = (REC.size() != 0) ? REC + RECSUFFIX : (getYYYYMMDD_HHMMSS() + RECSUFFIX + ".rec");
                recFile.reset(new std::fstream(nameOfRecFile.c_str(), std::ios::out|std::ios::binary|std::ios::trunc));
                std::cout << "[opendlv-video-camera-rtsp]: Created " << nameOfRecFile << "." << std::endl;
              }
              else if (2 == rc.command()) {
                if (recFile && recFile->good()) {
                  recFile->flush();
                  recFile->close();
                  std::cout << "[opendlv-video-camera-rtsp]: Closed " << nameOfRecFile << "." << std::endl;
                }
                recFile = nullptr;
              }
            }
            else {
              std::lock_guard<std::mutex> lck(recFileMutex);
              if (recFile && recFile->good()) {
                std::string serializedData{cluon::serializeEnvelope(std::move(envelope))};
                recFile->write(serializedData.data(), serializedData.size());
                recFile->flush();
              }
            }
          }));
        }

        // Install signal handler to handle Ctrl-C.    
        signal(SIGINT, sig_handler);

        // Begin by setting up our usage environment:
        TaskScheduler* scheduler = BasicTaskScheduler::createNew();
        UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

        openURL(*env, argv[0], URL.c_str(), TRANSPORT_OVER_TCP, ID, VERBOSE, recFileMutex, recFile);

        env->taskScheduler().doEventLoop(&eventLoopWatchVariable);

        env->reclaim(); env = NULL;
        delete scheduler; scheduler = NULL;
        retCode = 0;
    }
    return retCode;
}

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  MediaSubsessionIterator* iter;
  MediaSession* session;
  MediaSubsession* subsession;
  TaskToken streamTimerTask;
  double duration;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient: public RTSPClient {
public:
  static ourRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
				  int verbosityLevel,
				  char const* applicationName,
                  bool TCP,
                  uint32_t senderStamp,
                  bool verbose,
                  std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile,
				  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
		int verbosityLevel, char const* applicationName, bool TCP,
        uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile,
        portNumBits tunnelOverHTTPPortNum);
    // called only by createNew();
  virtual ~ourRTSPClient();

public:
  bool m_TCP{false};
  uint32_t m_senderStamp{0};
  bool m_verbose{false};
  std::mutex &m_recFileMutex;
  std::unique_ptr<std::fstream> &m_recFile;
  StreamClientState scs;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.

class DummySink: public MediaSink {
public:
  static DummySink* createNew(UsageEnvironment& env,
			      MediaSubsession& subsession, // identifies the kind of data that's being received
			      char const* streamId,
                  uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile); // identifies the stream itself (optional)

private:
  DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile);
    // called only by "createNew()"
  virtual ~DummySink();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
				struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
			 struct timeval presentationTime, unsigned durationInMicroseconds);
  std::string addData(unsigned char* buffer, ssize_t size, struct timeval presentationTime);


private:
  // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  uint32_t m_senderStamp{0};
  bool m_verbose{false};
  std::mutex &m_recFileMutex;
  std::unique_ptr<std::fstream> &m_recFile;
  u_int8_t* fReceiveBuffer;
  MediaSubsession& fSubsession;
  char* fStreamId;
  uint32_t m_width{0};
  uint32_t m_height{0};
  bool m_wroteHeader{false};
};

#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

static unsigned rtspClientCount = 0; // Counts how many streams (i.e., "RTSPClient"s) are currently in use.

void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL, bool TCP, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile) {
  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  RTSPClient* rtspClient = ourRTSPClient::createNew(env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, progName, TCP, senderStamp, verbose, recFileMutex, recFile);
  if (rtspClient == NULL) {
    env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
    return;
  }

  ++rtspClientCount;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE); 
}


// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
    env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL) {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!scs.session->hasSubsessions()) {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

void setupNextSubsession(RTSPClient* rtspClient) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias
  
  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL) {
    if (!scs.subsession->initiate()) {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    } else {
      env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
      if (scs.subsession->rtcpIsMuxed()) {
	env << "client port " << scs.subsession->clientPortNum();
      } else {
	env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
      }
      env << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, ((ourRTSPClient*)rtspClient)->m_TCP);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL) {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  } else {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
    if (scs.subsession->rtcpIsMuxed()) {
      env << "client port " << scs.subsession->clientPortNum();
    } else {
      env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
    }
    env << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    scs.subsession->sink = DummySink::createNew(env, *scs.subsession, rtspClient->url(), ((ourRTSPClient*)rtspClient)->m_senderStamp, ((ourRTSPClient*)rtspClient)->m_verbose, ((ourRTSPClient*)rtspClient)->m_recFileMutex, ((ourRTSPClient*)rtspClient)->m_recFile);
      // perhaps use your own custom "MediaSink" subclass instead
    if (scs.subsession->sink == NULL) {
      env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
	  << "\" subsession: " << env.getResultMsg() << "\n";
      break;
    }

    env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
    scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
    scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
				       subsessionAfterPlaying, scs.subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (scs.subsession->rtcpInstance() != NULL) {
      scs.subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, scs.subsession);
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
  setupNextSubsession(rtspClient);
}

void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean success = False;

  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0) {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0) {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
  }
}


// Implementation of the other event handlers:

void subsessionAfterPlaying(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void* clientData, char const* reason) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  UsageEnvironment& env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\"";
  if (reason != NULL) {
    env << " (reason:\"" << reason << "\")";
    delete[] (char*)reason;
  }
  env << " on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  StreamClientState& scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient* rtspClient, int exitCode) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession* subsession;

    while ((subsession = iter.next()) != NULL) {
      if (subsession->sink != NULL) {
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	if (subsession->rtcpInstance() != NULL) {
	  subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
	}

	someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
    // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

  if (--rtspClientCount == 0) {
    // The final stream has ended, so exit the application now.
    // (Of course, if you're embedding this code into your own application, you might want to comment this out,
    // and replace it with "eventLoopWatchVariable = 1;", so that we leave the LIVE555 event loop, and continue running "main()".)
    exit(exitCode);
  }
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
					int verbosityLevel, char const* applicationName, bool TCP, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, TCP, senderStamp, verbose, recFileMutex, recFile, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
			     int verbosityLevel, char const* applicationName, bool TCP, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1)
  , m_TCP(TCP)
  , m_senderStamp(senderStamp)
  , m_verbose(verbose)
  , m_recFileMutex(recFileMutex)
  , m_recFile(recFile) {
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}


// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 10*1024*1024

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile) {
  return new DummySink(env, subsession, streamId, senderStamp, verbose, recFileMutex, recFile);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId, uint32_t senderStamp, bool verbose, std::mutex &recFileMutex, std::unique_ptr<std::fstream> &recFile)
  : MediaSink(env)
  , m_senderStamp(senderStamp)
  , m_verbose(verbose)
  , m_recFileMutex(recFileMutex)
  , m_recFile(recFile)
  , fSubsession(subsession) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
}

DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

std::string DummySink::addData(unsigned char* buffer, ssize_t size, struct timeval presentationTime) {
    cluon::data::TimeStamp ts;
    if (presentationTime.tv_sec == 0) {
        ts = cluon::time::now();
    }
    else {
        ts.seconds(presentationTime.tv_sec).microseconds(presentationTime.tv_usec);
    }

    std::string _outData(reinterpret_cast<char*>(buffer), size);

    opendlv::proxy::ImageReading ir;
    ir.fourcc("h264").width(m_width).height(m_height).data(_outData);
    cluon::data::Envelope envelope;
    {
        cluon::ToProtoVisitor protoEncoder;
        {
          envelope.dataType(ir.ID());
          ir.accept(protoEncoder);
          envelope.serializedData(protoEncoder.encodedData());
          envelope.sent(cluon::time::now());
          envelope.sampleTimeStamp(ts);
          envelope.senderStamp(m_senderStamp);
        }
    }
    std::string serializedData{cluon::serializeEnvelope(std::move(envelope))};
    return serializedData;
}


void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  if (m_verbose) {
      // We've just received a frame of data.  (Optionally) print out information about it:
      if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
      envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
      if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
      char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
      sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
      envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
      if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
        envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
      }
  }
  {
      std::lock_guard<std::mutex> lck(m_recFileMutex);
      if (m_recFile && m_recFile->good()) {
        unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01};
        if (!m_wroteHeader) {
          char const* prop = fSubsession.fmtp_spropparametersets();
          unsigned numSPropRecords{0};
          SPropRecord* sPropRecords = parseSPropParameterSets(prop, numSPropRecords);

          struct FrameDimension fd = Parse(sPropRecords[0].sPropBytes, sPropRecords[0].sPropLength);
          m_width = fd.width;
          m_height = fd.height;
          std::cout << "Found video stream: " << m_width << "x" << m_height << std::endl;

         for (unsigned i = 0; i < numSPropRecords; ++i) {
            unsigned char *bn = new unsigned char[sPropRecords[i].sPropLength + 4];
            if (sPropRecords[i].sPropLength > 0) {
                memcpy(bn, start_code, 4);
            }
            memcpy(bn+4, sPropRecords[i].sPropBytes, sPropRecords[i].sPropLength);
            std::string serializedData = addData(bn, 4 + sPropRecords[i].sPropLength, presentationTime);
            m_recFile->write((char*)serializedData.c_str(), serializedData.size());
            delete [] bn;
          }
     
          m_wroteHeader = true;
        }
        unsigned char *bn = new unsigned char[frameSize + 4];
        memcpy(bn, start_code, 4);
        memcpy(bn+4, fReceiveBuffer, frameSize);
        std::string serializedData = addData(bn, frameSize+4, presentationTime);
        m_recFile->write((char*)serializedData.c_str(), serializedData.size());
        m_recFile->flush();
        delete [] bn;
      }
      else {
          m_wroteHeader = false;
      }
  }
  if (m_verbose) {
    envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
    envir() << "\n";
  } 
  
  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}

