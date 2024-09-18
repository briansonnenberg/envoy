#pragma once

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "envoy/common/pure.h"
#include "envoy/network/transport_socket.h"
#include "envoy/ssl/context.h"
#include "envoy/ssl/context_config.h"
#include "envoy/ssl/private_key/private_key.h"
#include "envoy/ssl/ssl_socket_extended_info.h"

#include "source/common/common/logger.h"
#include "source/common/common/c_smart_ptr.h"
#include "source/common/common/matchers.h"
#include "source/common/stats/symbol_table.h"
#include "source/common/tls/cert_validator/cert_validator.h"
#include "source/common/tls/cert_validator/san_matcher.h"
#include "source/common/tls/stats.h"


#include "openssl/ssl.h"
#include "openssl/x509v3.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Tls {

using X509StorePtr = CSmartPtr<X509_STORE, X509_STORE_free>;

struct SpiffeData {
  absl::flat_hash_map<std::string, CSmartPtr<X509_STORE, X509_STORE_free>> trust_bundle_stores;
  std::vector<bssl::UniquePtr<X509>> ca_certs;

  int64_t spiffe_refresh_hint;
  int64_t spiffe_sequence;

  SpiffeData() = default;
  SpiffeData(const SpiffeData& other) = delete;
  SpiffeData(SpiffeData&& other) = default;
};

class SPIFFEValidator : public CertValidator, Logger::Loggable<Logger::Id::secret> {
public:
  SPIFFEValidator(SslStats& stats, Server::Configuration::CommonFactoryContext& context)
      : tls_(ThreadLocal::TypedSlot<ThreadLocalSpiffeState>::makeUnique(context.threadLocal())),
        spiffe_data_(std::make_shared<SpiffeData>()),
        main_thread_dispatcher_(context.mainThreadDispatcher()),
        stats_(stats),
        time_source_(context.timeSource()) {
            tls_->set([](Event::Dispatcher&) {
              return std::make_shared<ThreadLocalSpiffeState>();
            });
            updateSpiffeData(spiffe_data_);
        };
  SPIFFEValidator(const Envoy::Ssl::CertificateValidationContextConfig* config, SslStats& stats,
                  Server::Configuration::CommonFactoryContext& context);

  ~SPIFFEValidator() override = default;

  // Tls::CertValidator
  absl::Status addClientValidationContext(SSL_CTX* context, bool require_client_cert) override;

  ValidationResults
  doVerifyCertChain(STACK_OF(X509)& cert_chain, Ssl::ValidateResultCallbackPtr callback,
                    const Network::TransportSocketOptionsConstSharedPtr& transport_socket_options,
                    SSL_CTX& ssl_ctx,
                    const CertValidator::ExtraValidationContext& validation_context, bool is_server,
                    absl::string_view host_name) override;

  absl::StatusOr<int> initializeSslContexts(std::vector<SSL_CTX*> contexts,
                                            bool provides_certificates) override;

  void updateDigestForSessionId(bssl::ScopedEVP_MD_CTX& md, uint8_t hash_buffer[EVP_MAX_MD_SIZE],
                                unsigned hash_length) override;

  absl::optional<uint32_t> daysUntilFirstCertExpires() const override;
  std::string getCaFileName() const override { return ca_file_name_; }
  Envoy::Ssl::CertificateDetailsPtr getCaCertInformation() const override;

  // Utility functions
  X509_STORE* getTrustBundleStore(X509* leaf_cert);
  static std::string extractTrustDomain(const std::string& san);
  static bool certificatePrecheck(X509* leaf_cert);
  std::shared_ptr<SpiffeData> getSpiffeData() {
    return spiffe_data_;
  };
  bool matchSubjectAltName(X509& leaf_cert);


private:
  bool verifyCertChainUsingTrustBundleStore(X509& leaf_cert, STACK_OF(X509)* cert_chain,
                                            X509_VERIFY_PARAM* verify_param,
                                            std::string& error_details);

  void initializeCertificateRefresh(Server::Configuration::CommonFactoryContext& context);
  std::shared_ptr<SpiffeData> loadTrustBundleMap();

  class ThreadLocalSpiffeState : public Envoy::ThreadLocal::ThreadLocalObject {
  public:
      std::shared_ptr<SpiffeData> getSpiffeData() const { return spiffe_data_; }
      void updateSpiffeData(std::shared_ptr<SpiffeData> new_data) {
        ENVOY_LOG(debug, "updating spiffe data");
        spiffe_data_ = new_data;
      }

  private:
      std::shared_ptr<SpiffeData> spiffe_data_;
  };

  void updateSpiffeDataAsync(std::shared_ptr<SpiffeData> new_spiffe_data) {
    ENVOY_LOG(debug, "Posting new SPIFFE data update to main thread dispatcher");
    main_thread_dispatcher_.post([this, new_spiffe_data]() {
        ENVOY_LOG(debug, "Updating spiffe_data_ for all threads");
        tls_->runOnAllThreads([new_spiffe_data](OptRef<ThreadLocalSpiffeState> obj) {
            ENVOY_LOG(debug, "loading new spiffe data");
            obj->updateSpiffeData(new_spiffe_data);
        });
    });
  };

  void updateSpiffeData(std::shared_ptr<SpiffeData> new_spiffe_data) {
    tls_->runOnAllThreads(
        [new_spiffe_data](OptRef<ThreadLocalSpiffeState> obj) {
            ENVOY_LOG(debug, "loading new spiffe data");
            obj->updateSpiffeData(new_spiffe_data);
        },
        []() {
            ENVOY_LOG(debug, "SPIFFE data update completed on all threads");
        }
    );
  }

  bool allow_expired_certificate_{false};

  ThreadLocal::TypedSlotPtr<ThreadLocalSpiffeState> tls_;
  std::string ca_file_name_;
  std::string trust_bundle_file_name_;
  std::shared_ptr<SpiffeData> spiffe_data_;
  std::vector<SanMatcherPtr> subject_alt_name_matchers_{};
  Event::Dispatcher& main_thread_dispatcher_;
  std::unique_ptr<Filesystem::Watcher> file_watcher_;

  SslStats& stats_;
  TimeSource& time_source_;
};

} // namespace Tls
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
