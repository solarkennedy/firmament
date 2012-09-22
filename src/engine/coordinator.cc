// The Firmament project
// Copyright (c) 2011-2012 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// Platform-independent coordinator class implementation. This is subclassed by
// the platform-specific coordinator classes.

#include "engine/coordinator.h"

#include <string>
#ifdef __PLATFORM_HAS_BOOST__
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#endif
#if (BOOST_VERSION < 104700)
#include <csignal>
#endif

#include "base/resource_desc.pb.h"
#include "messages/base_message.pb.h"
#include "messages/heartbeat_message.pb.h"
#include "messages/registration_message.pb.h"
#include "misc/protobuf_envelope.h"
#include "misc/map-util.h"
#include "misc/utils.h"

DEFINE_string(platform, "AUTO", "The platform we are running on, or AUTO for "
              "attempting automatic discovery.");
DEFINE_string(listen_uri, "tcp://localhost:9998",
              "The name/address/port to listen on.");
#ifdef __HTTP_UI__
DEFINE_int32(http_ui_port, 8080,
             "The port that the HTTP UI will be served on; -1 to disable.");
#endif

namespace firmament {

// Initial value of exit_ toggle
bool Coordinator::exit_ = false;

Coordinator::Coordinator(PlatformID platform_id)
  : platform_id_(platform_id),
    uuid_(GenerateUUID()),
    topology_manager_(new TopologyManager())
#if (BOOST_VERSION >= 104700)
    , signal_set_(signal_set(signal_io_service_)) {
#else
    {
#endif
  // Start up a coordinator ccording to the platform parameter
  // platform_ = platform::GetByID(platform_id);
  string desc_name = "";  // platform_.GetDescriptiveName();
  resource_desc_.set_uuid(to_string(uuid_));

  // Log information
  LOG(INFO) << "Coordinator starting on host " << FLAGS_listen_uri
            << ", platform " << platform_id << ", uuid " << uuid_;

  switch (platform_id) {
    case PL_UNIX: {
      m_adapter_.reset(
          new platform_unix::streamsockets::
          StreamSocketsAdapter<BaseMessage>());
#if (BOOST_VERSION >= 104700)
      signals_.add(SIGINT);
      signals_.add(SIGTERM);
      signals_.async_wait(boost::bind(&Cooordinator::HandleSignal, this));
#else
      signal(SIGINT, Coordinator::HandleSignal);
      signal(SIGTERM, Coordinator::HandleSignal);
#endif
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented!";
  }

#ifdef __HTTP_UI__
  // Start up HTTP interface
  if (FLAGS_http_ui_port > 0) {
    // TODO(malte): This is a hack to avoid failure of shared_from_this()
    // because we do not have a shared_ptr to this object yet. Not sure if this
    // is safe, though.... (I think it is, as long as the Coordinator's main()
    // still holds a shared_ptr to the Coordinator).
    shared_ptr<Coordinator> dummy(this);
    c_http_ui_.reset(new CoordinatorHTTPUI(shared_from_this()));
    c_http_ui_->Init(FLAGS_http_ui_port);
  }
#endif

  // test topology detection
  topology_manager_->DebugPrintRawTopology();
}

void Coordinator::Run() {
  // Coordinator starting -- set up and wait for workers to connect.
  m_adapter_->Listen(FLAGS_listen_uri);
  m_adapter_->RegisterAsyncMessageReceiptCallback(
      boost::bind(&Coordinator::HandleIncomingMessage, this, _1));
  while (!exit_) {  // main loop
    // Wait for events (i.e. messages from workers)
    // TODO(malte): we need to think about any actions that the coordinator
    // itself might need to take, and how they can be triggered
    VLOG(2) << "Hello from main loop!";
    AwaitNextMessage();
  }

  // We have dropped out of the main loop and are exiting
  // TODO(malte): any cleanup we need to do; hand-over to another coordinator if
  // possible?
  Shutdown("dropped out of main loop");
}

void Coordinator::AwaitNextMessage() {
  VLOG(2) << "Waiting for next message from adapter...";
  m_adapter_->AwaitNextMessage();
  boost::this_thread::sleep(boost::posix_time::seconds(1));
}

void Coordinator::HandleRecv(const boost::system::error_code& error,
                             size_t bytes_transferred,
                             Envelope<BaseMessage>* env) {
  if (error) {
    LOG(WARNING) << "Asynchronous receive call returned an error: "
                 << error.message();
    return;
  }
  VLOG(2) << "Received " << bytes_transferred << " bytes asynchronously, "
          << "in envelope at " << env << ", representing message " << *env;
  BaseMessage *bm = env->data();
  HandleIncomingMessage(bm);
  delete env;
}

void Coordinator::HandleIncomingMessage(BaseMessage *bm) {
  // Registration message
  if (bm->HasExtension(register_extn)) {
    const RegistrationMessage& msg = bm->GetExtension(register_extn);
    HandleRegistrationRequest(msg);
  }
  // Heartbeat message
  if (bm->HasExtension(heartbeat_extn)) {
    const HeartbeatMessage& msg = bm->GetExtension(heartbeat_extn);
    HandleHeartbeat(msg);
  }
}

void Coordinator::HandleHeartbeat(const HeartbeatMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  pair<ResourceDescriptor, uint64_t>* rdp =
      FindOrNull(associated_resources_, uuid);
  if (!rdp) {
    LOG(WARNING) << "HEARTBEAT from UNKNOWN resource (uuid: "
                 << msg.uuid() << ")!";
  } else {
    LOG(INFO) << "HEARTBEAT from resource " << msg.uuid()
              << " (last seen at " << rdp->second << ")";
    // Update timestamp
    rdp->second = GetCurrentTimestamp();
  }
}

void Coordinator::HandleRegistrationRequest(
    const RegistrationMessage& msg) {
  boost::uuids::string_generator gen;
  boost::uuids::uuid uuid = gen(msg.uuid());
  pair<ResourceDescriptor, uint64_t>* rdp =
      FindOrNull(associated_resources_, uuid);
  if (!rdp) {
    LOG(INFO) << "REGISTERING NEW RESOURCE (uuid: " << msg.uuid() << ")";
    // N.B.: below will copy the resource descriptor
    CHECK(InsertIfNotPresent(&associated_resources_, uuid,
                             pair<ResourceDescriptor, uint64_t>(
                                 msg.res_desc(),
                                 GetCurrentTimestamp())));
  } else {
    LOG(INFO) << "REGISTRATION request from resource " << msg.uuid()
              << " that we already know about. "
              << "Checking if this is a recovery.";
    // TODO(malte): Implement checking logic, deal with recovery case
    // Update timestamp (registration request is an implicit heartbeat)
    rdp->second = GetCurrentTimestamp();
  }
}


#if (BOOST_VERSION < 104700)
void Coordinator::HandleSignal(int) {
#else
void Coordinator::HandleSignal() {
#endif
  // XXX(malte): stub
  exit_ = true;
}

ResourceID_t Coordinator::GenerateUUID() {
  boost::uuids::random_generator gen;
  return gen();
}

const string Coordinator::SubmitJob(const JobDescriptor& job_descriptor) {
  LOG(INFO) << "NEW JOB: " << job_descriptor.DebugString();
  return "test1234";
}

void Coordinator::Shutdown(const string& reason) {
  LOG(INFO) << "Coordinator shutting down; reason: " << reason;
#ifdef __HTTP_UI__
  //c_http_ui_->Shutdown();
#endif
  m_adapter_->StopListen();
  // Toggling the exit flag will make the Coordinator drop out of its main loop.
  exit_ = true;
}

}  // namespace firmament
