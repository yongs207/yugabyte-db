// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#include "yb/master/master_tablet_service.h"

#include "yb/master/catalog_manager-internal.h"

namespace yb {
namespace master {

MasterTabletServiceImpl::MasterTabletServiceImpl(MasterTabletServer* server, Master* master)
    : TabletServiceImpl(server), master_(master) {
}

bool MasterTabletServiceImpl::GetTabletOrRespond(const tserver::ReadRequestPB* req,
                                                 tserver::ReadResponsePB* resp,
                                                 rpc::RpcContext* context,
                                                 std::shared_ptr<tablet::AbstractTablet>* tablet) {
  CatalogManager::ScopedLeaderSharedLock l(master_->catalog_manager());
  if (!l.CheckIsInitializedAndIsLeaderOrRespondTServer(resp, context)) {
    return false;
  }

  const auto result = master_->catalog_manager()->GetSystemTablet(req->tablet_id());
  if (PREDICT_FALSE(!result)) {
    tserver::TabletServerErrorPB* error = resp->mutable_error();
    StatusToPB(result.status(), error->mutable_status());
    error->set_code(tserver::TabletServerErrorPB::TABLET_NOT_FOUND);
    context->RespondSuccess();
    return false;
  }

  *tablet = *result;
  return true;
}

namespace {

void HandleUnsupportedMethod(const char* method_name, rpc::RpcContext* context) {
  context->RespondRpcFailure(rpc::ErrorStatusPB::ERROR_APPLICATION,
                             STATUS_FORMAT(NotSupported, "$0 Not Supported!", method_name));
}

} // namespace

void MasterTabletServiceImpl::ListTablets(const tserver::ListTabletsRequestPB* req,
                                          tserver::ListTabletsResponsePB* resp,
                                          rpc::RpcContext context)  {
  HandleUnsupportedMethod("ListTablets", &context);
}

void MasterTabletServiceImpl::ListTabletsForTabletServer(
    const tserver::ListTabletsForTabletServerRequestPB* req,
    tserver::ListTabletsForTabletServerResponsePB* resp,
    rpc::RpcContext context)  {
  HandleUnsupportedMethod("ListTabletsForTabletServer", &context);
}

void MasterTabletServiceImpl::GetLogLocation(const tserver::GetLogLocationRequestPB* req,
                                             tserver::GetLogLocationResponsePB* resp,
                                             rpc::RpcContext context)  {
  HandleUnsupportedMethod("GetLogLocation", &context);
}

void MasterTabletServiceImpl::Checksum(const tserver::ChecksumRequestPB* req,
                                       tserver::ChecksumResponsePB* resp,
                                       rpc::RpcContext context)  {
  HandleUnsupportedMethod("Checksum", &context);
}

} // namespace master
} // namespace yb
