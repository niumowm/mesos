/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <map>
#include <sstream>
#include <vector>

#include <stout/fatal.hpp>
#include <stout/foreach.hpp>

#include "local.hpp"

#include "configurator/configuration.hpp"
#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/drf_sorter.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

using namespace mesos::internal;

using mesos::internal::master::Allocator;
using mesos::internal::master::AllocatorProcess;
using mesos::internal::master::DRFSorter;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Isolator;
using mesos::internal::slave::ProcessIsolator;

using process::PID;
using process::UPID;

using std::map;
using std::string;
using std::stringstream;
using std::vector;


namespace mesos {
namespace internal {
namespace local {

static Allocator* allocator = NULL;
static AllocatorProcess* allocatorProcess = NULL;
static Master* master = NULL;
static map<Isolator*, Slave*> slaves;
static MasterDetector* detector = NULL;
static Files* files = NULL;

PID<Master> launch(
    int numSlaves,
    double cpus,
    uint64_t mem,
    uint64_t disk,
    bool quiet,
    Allocator* _allocator)
{
  Configuration configuration;
  configuration.set("slaves", "*");
  configuration.set("num_slaves", numSlaves);
  configuration.set("quiet", quiet);

  stringstream out;
  out << "cpus:" << cpus << ";" << "mem:" << mem << ";" << "disk:" << disk;
  configuration.set("resources", out.str());

  return launch(configuration, _allocator);
}


PID<Master> launch(const Configuration& configuration, Allocator* _allocator)
{
  int numSlaves = configuration.get<int>("num_slaves", 1);

  if (master != NULL) {
    LOG(FATAL) << "Can only launch one local cluster at a time (for now)";
  }

  if (_allocator == NULL) {
    // Create default allocator, save it for deleting later.
    allocatorProcess = new HierarchicalDRFAllocatorProcess();
    _allocator = allocator = new Allocator(allocatorProcess);
  } else {
    // TODO(benh): Figure out the behavior of allocator pointer and remove the
    // else block.
    allocator = NULL;
    allocatorProcess = NULL;
  }

  files = new Files();

  {
    master::Flags flags;
    flags.load(configuration.getMap());
    master = new Master(_allocator, files, flags);
  }

  PID<Master> pid = process::spawn(master);

  vector<UPID> pids;

  slave::Flags flags;
  flags.load(configuration.getMap());

  for (int i = 0; i < numSlaves; i++) {
    // TODO(benh): Create a local isolator?
    ProcessIsolator* isolator = new ProcessIsolator();

    // Use a different work directory for each slave.
    flags.work_dir = path::join(flags.work_dir, stringify(i));

    Slave* slave = new Slave(flags, true, isolator, files);
    slaves[isolator] = slave;
    pids.push_back(process::spawn(slave));
  }

  detector = new BasicMasterDetector(pid, pids, true);

  return pid;
}


void shutdown()
{
  if (master != NULL) {
    process::terminate(master->self());
    process::wait(master->self());
    delete master;
    delete allocator;
    delete allocatorProcess;
    master = NULL;

    // TODO(benh): Ugh! Because the isolator calls back into the slave
    // (not the best design) we can't delete the slave until we have
    // deleted the isolator. But since the slave calls into the
    // isolator, we can't delete the isolator until we have stopped
    // the slave.

    foreachpair (Isolator* isolator, Slave* slave, slaves) {
      process::terminate(slave->self());
      process::wait(slave->self());
      delete isolator;
      delete slave;
    }

    slaves.clear();

    delete detector;
    detector = NULL;

    delete files;
    files = NULL;
  }
}

} // namespace local {
} // namespace internal {
} // namespace mesos {
