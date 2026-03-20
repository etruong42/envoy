#include "source/extensions/filters/http/grpc_json_transcoder/filter_config.h"

#include <memory>
#include <unordered_set>

#include "envoy/common/exception.h"
#include "envoy/extensions/filters/http/grpc_json_transcoder/v3/transcoder.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/utility.h"
#include "source/common/grpc/common.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"

#include "absl/strings/str_join.h"
#include "google/api/annotations.pb.h"
#include "google/api/http.pb.h"
#include "google/api/httpbody.pb.h"
#include "grpc_transcoding/json_request_translator.h"
#include "grpc_transcoding/path_matcher_utility.h"
#include "grpc_transcoding/response_to_json_translator.h"

using absl::Status;
using absl::StatusCode;
using Envoy::Protobuf::FileDescriptorSet;
using Envoy::Protobuf::io::ZeroCopyInputStream;
using google::api::HttpRule;
using google::grpc::transcoding::JsonRequestTranslator;
using JsonRequestTranslatorPtr = std::unique_ptr<JsonRequestTranslator>;
using google::grpc::transcoding::MessageStream;
using google::grpc::transcoding::PathMatcherBuilder;
using google::grpc::transcoding::PathMatcherUtility;
using google::grpc::transcoding::RequestMessageTranslator;
using RequestMessageTranslatorPtr = std::unique_ptr<RequestMessageTranslator>;
using google::grpc::transcoding::ResponseToJsonTranslator;
using ResponseToJsonTranslatorPtr = std::unique_ptr<ResponseToJsonTranslator>;
using google::grpc::transcoding::Transcoder;
using TranscoderPtr = std::unique_ptr<Transcoder>;
using google::grpc::transcoding::TranscoderInputStream;
using TranscoderInputStreamPtr = std::unique_ptr<TranscoderInputStream>;
using envoy::extensions::filters::http::grpc_json_transcoder::v3::UnknownQueryParams;
using google::grpc::transcoding::VariableBinding;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcJsonTranscoder {

namespace {
    
}

} // namespace GrpcJsonTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
