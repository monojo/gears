// Copyright 2008, Google Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  3. Neither the name of Google Inc. nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#if defined(OS_MACOSX)

#ifdef USING_CCTESTS

#include "gears/base/common/ipc_message_queue_test.h"

#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "gears/base/common/common.h"
#include "gears/base/common/paths.h"
#include "gears/base/common/stopwatch.h"
#include "gears/base/common/string_utils.h"

const char16 *kIpcTestApp = STRING16(L"ipc_test");

//-----------------------------------------------------------------------------
// Master process test code
//-----------------------------------------------------------------------------

bool WaitForRegisteredProcesses(int n, int timeout) {
  // Not supported.
  assert(false);
  return false;
}

bool ValidateRegisteredProcesses(int n, const IpcProcessId *process_ids) {
  // Not supported.
  assert(false);
  return false;
}

bool MasterMessageHandler::WaitForMessages(int n) {
  assert(save_messages_);

  // Note: 
  // 1) The IPC implementation for MacOSX is based on
  //    NSDistributedNotificationCenter which delivers notifications to the
  //    main thread. We have to allow the message loop to deal with the
  //    incoming events so that the notifications can be processed.
  // 2) The IPC testing code might run from main thread or different worker
  //    thread. So we should use the fine-grained lock here to avoid deadlock.
  {
    MutexLock locker(&lock_);
    num_messages_waiting_to_receive_ = n;
    wait_for_messages_start_time_ = GetCurrentTimeMillis();
  }
  while (true) {
    {
      MutexLock locker(&lock_);
      if (HasMessagesOrTimedout()) {
        if (num_received_messages_ == num_messages_waiting_to_receive_) {
          return true;
        }
        break;
      }
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
  }
  return false;
}

std::string GetSlaveInfoPath(int process_id) {
  // TODO (jianli): switch to using GetUserTempDirectory().
  char path[256] = {0};
  snprintf(path, sizeof(path), "/tmp/%s%d.ipctest",
           PRODUCT_SHORT_NAME_ASCII, process_id);
  return path;
}

bool GetSlavePath(std::string16 *slave_path) {
  assert(slave_path);

#ifndef BROWSER_NONE
  std::string16 install_directory;
  if (!GetComponentDirectory(&install_directory)) {
    LOG(("Couldn't get install directory\n"));
    return false;
  }

  slave_path->append(install_directory);
  AppendName(kIpcTestApp, slave_path);
#endif  // !BROWSER_NONE  

  return true;
}

bool SlaveProcess::Start(bool as_peer) {
  // Only support system IPC queue.
  assert(!as_peer);

  std::string16 slave_path;
  if (!GetSlavePath(&slave_path)) {
    return false;
  }

  std::string narrow_slave_path;
  if (!String16ToUTF8(slave_path.c_str(), &narrow_slave_path)) {
    return false;
  }

  NSString *ns_slave_path =
      [[NSString alloc] initWithCString:narrow_slave_path.c_str()
                               encoding:NSUTF8StringEncoding];
  NSArray* args = [NSArray array];                                             
  NSTask *task = [NSTask launchedTaskWithLaunchPath:ns_slave_path
                                          arguments:args];
  if (!task) {
    return false;
  }
  
  id_ = [task processIdentifier];
  return true;
}

bool SlaveProcess::WaitTillRegistered(int timeout) {
  return true;
}

bool SlaveProcess::WaitForExit(int timeout) {
  assert(id_);

  LOG(("Waiting for process %u\n", id_));
  slave_processes_.remove(id_);
  while (timeout > 0) {
    if (kill(id_, 0) == 0) {
      return true;
    }
    usleep(1000);
    timeout--;
  }
  return false;
}


//-----------------------------------------------------------------------------
// Slave process test code
//-----------------------------------------------------------------------------

static NSAutoreleasePool *ns_pool = nil;
static SlaveMessageHandler g_slave_handler;

IpcMessageQueue *SlaveMessageHandler::GetIpcMessageQueue() const {
  return IpcMessageQueue::GetSystemQueue();
}

void SlaveMessageHandler::TerminateSlave() {
  LOG(("Terminating slave process %u\n", getpid()));
  g_slave_handler.set_done(true);
  [NSApp terminate:NSApp];
}

bool InitSlave() {
  LOG(("Initializing slave process %u\n", getpid()));

  // Initialize the application.
  ns_pool = [[NSAutoreleasePool alloc] init];
  [NSApplication sharedApplication];
  if (!NSApplicationLoad()) {
    return false;
  }

  // Create the new ipc message queue for the child process.
  IpcTestMessage::RegisterAsSerializable();
  IpcMessageQueue *ipc_message_queue = g_slave_handler.GetIpcMessageQueue();
  if (!ipc_message_queue) {
    return false;
  }
  ipc_message_queue->RegisterHandler(kIpcQueue_TestMessage, &g_slave_handler);

  g_slave_handler.set_parent_process_id(getppid());

  // Send a hello message to the master process to tell that the child process
  // has been started.
  ipc_message_queue->Send(g_slave_handler.parent_process_id(),
                          kIpcQueue_TestMessage,
                          new IpcTestMessage(GetHelloMessage()));

  LOG(("Slave process sent hello message to master process %u\n",
       g_slave_handler.parent_process_id()));

  return true;
}

void RunSlave() {
  LOG(("Running slave process %u\n", getpid()));
  [NSApp run];
  [ns_pool release];
  LOG(("Slave process %u exitting\n", getpid()));
}

int SlaveMain() {
  if (!InitSlave()) {
    return 1;
  }
  RunSlave();
  return 0;
}

#ifdef BROWSER_NONE

extern "C"
int main(int argc, char **argv) {
  return SlaveMain();
}

#endif  // BROWSER_NONE

#endif  // USING_CCTESTS

#endif  // defined(OS_MACOSX)
