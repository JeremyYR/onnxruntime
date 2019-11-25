// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "inc/WinMLAdapter.h"
#include "inc/CustomRegistryHelper.h"
#include "PheonixSingleton.h"
#include "inc/LotusEnvironment.h"
#include "inc/AbiCustomRegistryImpl.h"
#include "core/providers/dml/DmlExecutionProvider/inc/DmlExecutionProvider.h"
#include "core/providers/dml/GraphTransformers/GraphTransformerHelpers.h"
#include "core/providers/dml/OperatorAuthorHelper/SchemaInferenceOverrider.h"

#include "LearningModelDevice.h"
#include "TensorFeatureDescriptor.h"
#include "ImageFeatureDescriptor.h"
#include "api.image/inc/D3DDeviceCache.h"

#include "PheonixSingleton.h"

#include "DmlOrtSessionBuilder.h"
#include "CpuOrtSessionBuilder.h"

#include <io.h>
#include <fcntl.h>

#include "ZeroCopyInputStreamWrapper.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#include "FeatureDescriptorFactory.h"

using namespace winrt::Windows::AI::MachineLearning;

namespace Windows::AI::MachineLearning::Adapter {

// ORT intentionally requires callers derive from their session class to access
// the protected methods used below.
class InferenceSessionProtectedLoadAccessor : public onnxruntime::InferenceSession {
 public:
  onnxruntime::common::Status
  Load(std::unique_ptr<ONNX_NAMESPACE::ModelProto> p_model_proto) {
    return onnxruntime::InferenceSession::Load(std::move(p_model_proto));
  }
  const onnxruntime::SessionState& GetSessionState() {
    return session_state_;
  }
};

class ModelProto : public Microsoft::WRL::RuntimeClass<
                       Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                       IModelProto> {
 public:
  ModelProto::ModelProto(onnx::ModelProto* model_proto) : model_proto_(model_proto) {
  }

  onnx::ModelProto* STDMETHODCALLTYPE get() override {
    return model_proto_.get();
  }

  onnx::ModelProto* STDMETHODCALLTYPE detach() override {
    return model_proto_.release();
  }

 private:
  std::unique_ptr<onnx::ModelProto> model_proto_;
};  // class ModelProto

class ModelInfo : public Microsoft::WRL::RuntimeClass<
                      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                      IModelInfo> {
 private:
  std::string author_;
  std::string name_;
  std::string domain_;
  std::string description_;
  int64_t version_;
  std::unordered_map<std::string, std::string> model_metadata_;
  wfc::IVector<winml::ILearningModelFeatureDescriptor> input_features_;
  wfc::IVector<winml::ILearningModelFeatureDescriptor> output_features_;

 public:
  ModelInfo(const onnx::ModelProto* model_proto) {
    Initialize(model_proto);
  }

  std::string& STDMETHODCALLTYPE author() override {
    return author_;
  }
  std::string& STDMETHODCALLTYPE name() override {
    return name_;
  }
  std::string& STDMETHODCALLTYPE domain() override {
    return domain_;
  }
  std::string& STDMETHODCALLTYPE description() override {
    return description_;
  }
  int64_t STDMETHODCALLTYPE version() override {
    return version_;
  }
  std::unordered_map<std::string, std::string>& STDMETHODCALLTYPE model_metadata() override {
    return model_metadata_;
  }
  wfc::IVector<winml::ILearningModelFeatureDescriptor>& STDMETHODCALLTYPE input_features() override {
    return input_features_;
  }
  wfc::IVector<winml::ILearningModelFeatureDescriptor>& STDMETHODCALLTYPE output_features() override {
    return output_features_;
  }

  static std::vector<const char*>
  GetAllNodeOutputs(const onnx::ModelProto& model_proto) {
    std::vector<const char*> nodes_outputs;
    auto& graph = model_proto.graph();
    auto& nodes = graph.node();
    for (auto& node : nodes) {
      for (auto& node_output : node.output()) {
        nodes_outputs.push_back(node_output.c_str());
      }
    }
    return nodes_outputs;
  }

  static std::vector<const char*>
  GetInitializers(const onnx::ModelProto& model_proto) {
    std::vector<const char*> initializers;
    auto& graph = model_proto.graph();
    auto& graph_initializers = graph.initializer();
    for (auto& initializer : graph_initializers) {
      initializers.push_back(initializer.name().c_str());
    }
    return initializers;
  }

  static std::vector<const onnx::ValueInfoProto*>
  GetInputsWithoutInitializers(const onnx::ModelProto& model_proto) {
    auto initializers = GetInitializers(model_proto);

    std::vector<const onnx::ValueInfoProto*> inputs_without_initializers;
    auto& graph = model_proto.graph();
    auto& inputs = graph.input();
    for (auto& input : inputs) {
      if (input.has_name() && input.has_type()) {
        auto found_it = std::find_if(
            std::begin(initializers),
            std::end(initializers),
            [&](auto& initializer) {
              return std::strcmp(initializer, input.name().c_str()) == 0;
            });

        auto is_initializer = found_it != std::end(initializers);
        if (!is_initializer) {
          inputs_without_initializers.push_back(&input);
        }
      }
    }
    return inputs_without_initializers;
  }

  static std::vector<const onnx::ValueInfoProto*> GetOutputs(const onnx::ModelProto& model_proto) {
    std::vector<const onnx::ValueInfoProto*> outputs_with_name;
    auto& graph = model_proto.graph();
    auto& outputs = graph.output();
    for (auto& output : outputs) {
      if (output.has_name() && output.has_type()) {
        outputs_with_name.push_back(&output);
      }
    }
    return outputs_with_name;
  }

 private:
  void Initialize(const onnx::ModelProto* model_proto) {
    // metadata
    for (auto& prop : model_proto->metadata_props()) {
      model_metadata_[prop.key()] = prop.value();
    }

    WinML::FeatureDescriptorFactory builder(model_metadata_);

    // Create inputs
    auto inputs = GetInputsWithoutInitializers(*model_proto);
    input_features_ = builder.CreateDescriptorsFromValueInfoProtos(inputs);

    // Create outputs
    auto outputs = GetOutputs(*model_proto);
    output_features_ = builder.CreateDescriptorsFromValueInfoProtos(outputs);

    // author
    auto has_producer_name = model_proto->has_producer_name();
    author_ = has_producer_name
                  ? model_proto->producer_name()
                  : "";

    // domain
    auto has_domain = model_proto->has_domain();
    domain_ = has_domain
                  ? model_proto->domain()
                  : "";

    // name
    auto has_graph = model_proto->has_graph();
    auto graph_has_name = model_proto->graph().has_name();
    auto is_name_available = has_graph && graph_has_name;
    name_ = is_name_available
                ? model_proto->graph().name()
                : "";

    // description
    auto has_description = model_proto->has_doc_string();
    description_ = has_description
                       ? model_proto->doc_string()
                       : "";

    // version
    auto has_version = model_proto->has_model_version();
    version_ = has_version
                   ? model_proto->model_version()
                   : 0;
  }
};  // class ModelInfo

class WinMLAdapter : public Microsoft::WRL::RuntimeClass<
                         Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                         IWinMLAdapter> {
 private:
  std::shared_ptr<WinML::LotusEnvironment> lotus_environment_;

 public:
  WinMLAdapter() : lotus_environment_(PheonixSingleton<WinML::LotusEnvironment>()) {
  }

  // factory methods for creating an ort model from a path
  HRESULT STDMETHODCALLTYPE CreateModelProto(
      const char* path,
      IModelProto** model_proto) override {
    int file_descriptor;
    _set_errno(0);  // clear errno
    _sopen_s(
        &file_descriptor,
        path,
        O_RDONLY | _O_SEQUENTIAL | _O_BINARY,
        _SH_DENYWR,
        _S_IREAD | _S_IWRITE);

    errno_t err = 0;
    _get_errno(&err);
    THROW_HR_IF_MSG(
        __HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        err == ENOENT,
        "File not found: %s",
        path);

    THROW_HR_IF_MSG(
        E_FAIL,
        0 > file_descriptor,
        "Failed");  //errno

    auto stream = google::protobuf::io::FileInputStream(file_descriptor);
    stream.SetCloseOnDelete(true);

    auto model_proto_inner = new onnx::ModelProto();
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        model_proto_inner->ParseFromZeroCopyStream(&stream) == false,
        "The stream failed to parse.");

    auto model_proto_outer = wil::MakeOrThrow<ModelProto>(model_proto_inner);
    return model_proto_outer.CopyTo(__uuidof(IModelProto), reinterpret_cast<void**>(model_proto));
  }

  // factory methods for creating an ort model from a stream
  HRESULT STDMETHODCALLTYPE CreateModelProto(
      ABI::Windows::Storage::Streams::IRandomAccessStreamReference* stream_reference,
      IModelProto** model_proto) override {
    ZeroCopyInputStreamWrapper wrapper(stream_reference);

    auto model_proto_inner = new onnx::ModelProto();
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        model_proto_inner->ParseFromZeroCopyStream(&wrapper) == false,
        "The stream failed to parse.");

    auto model_proto_outer = wil::MakeOrThrow<ModelProto>(model_proto_inner);
    return model_proto_outer.CopyTo(__uuidof(IModelProto), reinterpret_cast<void**>(model_proto));
  }

  // factory methods for creating an ort model from a model_proto
  HRESULT STDMETHODCALLTYPE CreateModelProto(IModelProto* model_proto_in, IModelProto** model_proto) override {
    auto model_proto_inner = new onnx::ModelProto(*model_proto_in->get());
    auto model_proto_outer = wil::MakeOrThrow<ModelProto>(model_proto_inner);
    return model_proto_outer.CopyTo(__uuidof(IModelProto), reinterpret_cast<void**>(model_proto));
  }

  HRESULT STDMETHODCALLTYPE CreateModelInfo(IModelProto* model_proto, IModelInfo** model_info) override {
    auto model_info_outer = wil::MakeOrThrow<ModelInfo>(model_proto->get());
    return model_info_outer.CopyTo(__uuidof(IModelInfo), reinterpret_cast<void**>(model_info));
  }

  void STDMETHODCALLTYPE EnableDebugOutput() override {
    WinML::CWinMLLogSink::EnableDebugOutput();
  }

  static bool IsFeatureDescriptorFp16(
      winml::ILearningModelFeatureDescriptor descriptor) {
    if (auto imageFeatureDescriptor = descriptor.try_as<winml::IImageFeatureDescriptor2>()) {
      return TensorKind::Float16 == imageFeatureDescriptor.TensorKind();
    }

    if (auto tensorFeatureDescriptor = descriptor.try_as<winml::ITensorFeatureDescriptor>()) {
      return TensorKind::Float16 == tensorFeatureDescriptor.TensorKind();
    }

    return false;
  }

  HRESULT STDMETHODCALLTYPE EnsureModelDeviceCompatibility(
      winml::LearningModel const& model,
      IModelProto* p_model_proto,
      bool is_float16_supported) override {
    if (!is_float16_supported) {
      auto& graph = p_model_proto->get()->graph();

      // The model will not contain fp16 operations if:
      // 1. The model has no fp16 inputs
      // 2. The model has no fp16 initializers
      // 3. The model does not create any fp16 intermediary tensors via the Cast (to float16) operator
      // 4. The model does not have any fp16 outputs

      // 1. Ensure that The model has no fp16 inputs
      for (auto descriptor : model.InputFeatures()) {
        THROW_HR_IF_MSG(
            DXGI_ERROR_UNSUPPORTED,
            IsFeatureDescriptorFp16(descriptor),
            "The model contains a 16-bit input (%ls), but the current device does not support 16-bit float.",
            descriptor.Name().c_str());
      }

      // 2. Ensure that the model has no fp16 initializers
      for (int i = 0; i < graph.node_size(); i++) {
        auto node = graph.node(i);
        if (node.op_type() == "Cast" && node.domain().empty()) {
          for (int attribIndex = 0; attribIndex < node.attribute_size(); attribIndex++) {
            auto attribute = node.attribute(attribIndex);
            if (attribute.name() == "to") {
              THROW_HR_IF_MSG(
                  DXGI_ERROR_UNSUPPORTED,
                  attribute.i() == onnx::TensorProto::DataType::TensorProto_DataType_FLOAT16,
                  "The model contains a 16-bit float Cast Op (%s), but the current device does not support 16-bit float.",
                  node.name().c_str());
            }
          }
        }
      }

      // 3. Ensure that the model does not create any fp16 intermediary
      //    tensors via the Cast (to float16) operator
      for (int i = 0; i < graph.initializer_size(); i++) {
        auto initializer = graph.initializer(i);

        THROW_HR_IF_MSG(
            DXGI_ERROR_UNSUPPORTED,
            initializer.data_type() == onnx::TensorProto::DataType::TensorProto_DataType_FLOAT16,
            "The model contains a 16-bit float initializer (%s), but the current device does not support 16-bit float.",
            initializer.name().c_str());
      }

      // 4. Ensure that the model does not have any fp16 outputs
      for (auto descriptor : model.OutputFeatures()) {
        THROW_HR_IF_MSG(
            DXGI_ERROR_UNSUPPORTED,
            IsFeatureDescriptorFp16(descriptor),
            "The model contains a 16-bit output (%ls), but the current device does not support 16-bit float.",
            descriptor.Name().c_str());
      }
    }
    return S_OK;
  }

  ID3D12Resource* STDMETHODCALLTYPE GetD3D12ResourceFromAllocation(onnxruntime::IExecutionProvider* provider, void* allocation) override {
    auto d3dResource =
        Dml::GetD3D12ResourceFromAllocation(
            provider->GetAllocator(0, ::OrtMemType::OrtMemTypeDefault).get(),
            allocation);
    return d3dResource;
  }

  static onnxruntime::MLDataType GetType(winml::TensorKind kind) {
    switch (kind) {
      case winml::TensorKind::Float:
        return onnxruntime::DataTypeImpl::GetType<float>();
      case winml::TensorKind::Float16:
        return onnxruntime::DataTypeImpl::GetType<onnxruntime::MLFloat16>();
    };
    return nullptr;
  }

  // factory method for creating an ortsessionbuilder from a device
  HRESULT STDMETHODCALLTYPE CreateOrtSessionBuilder(
      ID3D12Device* device,
      ID3D12CommandQueue* queue,
      IOrtSessionBuilder** session_builder) override {
    if (device == nullptr) {
      auto builder = wil::MakeOrThrow<CpuOrtSessionBuilder>();
      return builder.CopyTo(__uuidof(IOrtSessionBuilder), reinterpret_cast<void**>(session_builder));
    } else {
      auto builder = wil::MakeOrThrow<DmlOrtSessionBuilder>(device, queue);
      return builder.CopyTo(__uuidof(IOrtSessionBuilder), reinterpret_cast<void**>(session_builder));
    }
  }

  HRESULT STDMETHODCALLTYPE GetMapType(const OrtValue* ort_value, ONNXTensorElementDataType* key_type, ONNXTensorElementDataType* value_type) override {
    *key_type = *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    auto type = ort_value->Type();
    if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapStringToString>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapStringToInt64>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapStringToFloat>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapStringToDouble>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapInt64ToString>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapInt64ToInt64>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapInt64ToFloat>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::MapInt64ToDouble>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetVectorMapType(const OrtValue* ort_value, ONNXTensorElementDataType* key_type, ONNXTensorElementDataType* value_type) override {
    *key_type = *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    auto type = ort_value->Type();
    if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::VectorMapStringToFloat>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    } else if (type == onnxruntime::DataTypeImpl::GetType<onnxruntime::VectorMapInt64ToFloat>()) {
      *key_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      *value_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCustomRegistry(IMLOperatorRegistry** registry) override {
    auto impl = wil::MakeOrThrow<AbiCustomRegistryImpl>();
    *registry = impl.Detach();
    return S_OK;
  }

  void* STDMETHODCALLTYPE CreateGPUAllocationFromD3DResource(ID3D12Resource* pResource) override {
    return Dml::CreateGPUAllocationFromD3DResource(pResource);
  }

  void STDMETHODCALLTYPE FreeGPUAllocation(void* ptr) override {
    Dml::FreeGPUAllocation(ptr);
  }
  HRESULT STDMETHODCALLTYPE CopyTensor(
      onnxruntime::IExecutionProvider* provider,
      OrtValue* src,
      OrtValue* dst) override {
    ORT_THROW_IF_ERROR(Dml::CopyTensor(
        provider,
        src->Get<onnxruntime::Tensor>(),
        *(dst->GetMutable<onnxruntime::Tensor>())));
    return S_OK;
  }

  // Override select shape inference functions which are incomplete in ONNX with versions that are complete,
  // and are also used in DML kernel registrations.  Doing this avoids kernel and shader creation being
  // deferred until first evaluation.  It also prevents a situation where inference functions in externally
  // registered schema are reachable only after upstream schema have been revised in a later OS release,
  // which would be a compatibility risk.
  HRESULT STDMETHODCALLTYPE OverrideSchemaInferenceFunctions() override {
    static std::once_flag schema_override_once_flag;
    std::call_once(schema_override_once_flag, []() {
      SchemaInferenceOverrider::OverrideSchemaInferenceFunctions();
    });
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetProviderMemoryInfo(
      onnxruntime::IExecutionProvider* provider,
      OrtMemoryInfo** memory_info) override {
    auto allocator = provider->GetAllocator(0, ::OrtMemType::OrtMemTypeDefault);

    const auto& info = allocator->Info();
    *memory_info = new OrtMemoryInfo(info.name, info.type, info.device, info.id, info.mem_type);
    if (*memory_info == nullptr) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetValueMemoryInfo(const OrtValue* ort_value, OrtMemoryInfo** memory_info) override {
    const auto& tensor = ort_value->Get<onnxruntime::Tensor>();
    auto info = tensor.Location();
    *memory_info = new OrtMemoryInfo(info.name, info.type, info.device, info.id, info.mem_type);
    if (*memory_info == nullptr) {
      return E_OUTOFMEMORY;
    }
    return S_OK;
  }

  struct AllocatorWrapper : public OrtAllocator {
   public:
    AllocatorWrapper(onnxruntime::AllocatorPtr impl) : impl_(impl) {
      version = ORT_API_VERSION;
      Alloc = AllocImpl;
      Free = FreeImpl;
      Info = InfoImpl;
    }

    static void* ORT_API_CALL AllocImpl(struct OrtAllocator* this_, size_t size) {
      return static_cast<AllocatorWrapper*>(this_)->impl_->Alloc(size);
    }
    static void ORT_API_CALL FreeImpl(struct OrtAllocator* this_, void* p) {
      return static_cast<AllocatorWrapper*>(this_)->impl_->Free(p);
    }
    static const struct OrtMemoryInfo* ORT_API_CALL InfoImpl(const struct OrtAllocator* this_) {
      return &(static_cast<const AllocatorWrapper*>(this_)->info_);
    }

   private:
    onnxruntime::AllocatorPtr impl_;
    OrtMemoryInfo info_;
  };

  HRESULT STDMETHODCALLTYPE GetProviderAllocator(
      onnxruntime::IExecutionProvider* provider,
      OrtAllocator** allocator) override {
    auto allocator_ptr = provider->GetAllocator(0, ::OrtMemType::OrtMemTypeDefault);
    *allocator = new AllocatorWrapper(allocator_ptr);
    if (*allocator == nullptr) {
      return E_OUTOFMEMORY;
    }

    return S_OK;
  }

};  // namespace Windows::AI::MachineLearning::Adapter

extern "C" HRESULT STDMETHODCALLTYPE OrtGetWinMLAdapter(IWinMLAdapter** adapter) {
  // make an adapter instance
  Microsoft::WRL::ComPtr<WinMLAdapter> adapterptr = wil::MakeOrThrow<WinMLAdapter>();
  return adapterptr.CopyTo(__uuidof(IWinMLAdapter), reinterpret_cast<void**>(adapter));
}

// class IOBinding
// ===============
class IOBinding : public Microsoft::WRL::RuntimeClass<
                      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                      IIOBinding> {
 private:
  std::shared_ptr<onnxruntime::IOBinding> binding_;
  std::vector<OrtValue*> outputs_weak_;

 public:
  IOBinding(onnxruntime::IOBinding* binding) : binding_(binding) {
  }

  onnxruntime::IOBinding* STDMETHODCALLTYPE get() override {
    return binding_.get();
  }

  HRESULT STDMETHODCALLTYPE BindInput(const std::string& name, OrtValue* ort_value) override {
    ORT_THROW_IF_ERROR(binding_->BindInput(name, *ort_value));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE BindOutput(const std::string& name, OrtValue* ort_value) override {
    // this can be null for unbound outputs
    if (ort_value == nullptr) {
      OrtValue empty_value = {};
      ORT_THROW_IF_ERROR(binding_->BindOutput(name, empty_value));
    } else {
      ORT_THROW_IF_ERROR(binding_->BindOutput(name, *ort_value));
    }
    return S_OK;
  }

  const std::vector<std::string>& STDMETHODCALLTYPE GetOutputNames() override {
    return binding_->GetOutputNames();
  }
  std::vector<OrtValue*>& STDMETHODCALLTYPE GetOutputs() override {
    auto& output_inner = binding_->GetOutputs();
    outputs_weak_.clear();
    for (unsigned i = 0; i < output_inner.size(); i++) {
      outputs_weak_.push_back(&(output_inner[i]));
    }
    return outputs_weak_;
  }
};

// InferenceSession
// ================

InferenceSession::InferenceSession(onnxruntime::InferenceSession* session) : session_(session) {
}

void STDMETHODCALLTYPE InferenceSession::RegisterGraphTransformers(bool registerLotusTransforms) {
  GraphTransformerHelpers::RegisterGraphTransformers(session_.get(), registerLotusTransforms);
}

HRESULT STDMETHODCALLTYPE InferenceSession::NewIOBinding(IIOBinding** io_binding) {
  std::unique_ptr<onnxruntime::IOBinding> binding;
  ORT_THROW_IF_ERROR(this->session_->NewIOBinding(&binding));
  auto io_binding_outer = wil::MakeOrThrow<IOBinding>(binding.release());
  return io_binding_outer.CopyTo(__uuidof(IIOBinding), reinterpret_cast<void**>(io_binding));
}

HRESULT STDMETHODCALLTYPE InferenceSession::Run(const onnxruntime::RunOptions* run_options, IIOBinding* io_binding) {
  ORT_THROW_IF_ERROR(this->session_->Run(*run_options, *(io_binding->get())));
  return S_OK;
}
HRESULT STDMETHODCALLTYPE InferenceSession::StartProfiling() {
  this->session_->StartProfiling(PheonixSingleton<WinML::LotusEnvironment>()->GetDefaultLogger());
  return S_OK;
}
HRESULT STDMETHODCALLTYPE InferenceSession::EndProfiling() {
  this->session_->EndProfiling();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
InferenceSession::LoadModel(
    IModelProto* model_proto) {
  auto session_protected_load_accessor =
      static_cast<InferenceSessionProtectedLoadAccessor*>(session_.get());
  // session's like to have their very own copy of the model_proto, use detach()
  std::unique_ptr<ONNX_NAMESPACE::ModelProto> model_proto_ptr(model_proto->detach());
  ORT_THROW_IF_ERROR(session_protected_load_accessor->Load(std::move(model_proto_ptr)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
InferenceSession::RegisterCustomRegistry(
    IMLOperatorRegistry* registry) {
  RETURN_HR_IF(S_OK, registry == nullptr);

  auto custom_registries = GetLotusCustomRegistries(registry);

  // Register
  for (auto& custom_registry : custom_registries) {
    ORT_THROW_IF_ERROR(session_->RegisterCustomRegistry(custom_registry));
  }

  return S_OK;
}

void STDMETHODCALLTYPE InferenceSession::FlushContext(onnxruntime::IExecutionProvider* dml_provider) {
  Dml::FlushContext(dml_provider);
}

void STDMETHODCALLTYPE InferenceSession::TrimUploadHeap(onnxruntime::IExecutionProvider* dml_provider) {
  Dml::TrimUploadHeap(dml_provider);
}

void STDMETHODCALLTYPE InferenceSession::ReleaseCompletedReferences(onnxruntime::IExecutionProvider* dml_provider) {
  Dml::ReleaseCompletedReferences(dml_provider);
}

HRESULT STDMETHODCALLTYPE InferenceSession::CopyOneInputAcrossDevices(
  const char* input_name,
  const OrtValue* orig_mlvalue, 
  OrtValue** new_mlvalue) {
  return E_NOTIMPL;
}

}  // namespace Windows::AI::MachineLearning::Adapter