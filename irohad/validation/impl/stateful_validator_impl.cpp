/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <numeric>
#include <set>

#include "backend/protobuf/from_old_model.hpp"
#include "builders/protobuf/proposal.hpp"
#include "model/account.hpp"
#include "validation/impl/stateful_validator_impl.hpp"

namespace iroha {
  namespace validation {

    StatefulValidatorImpl::StatefulValidatorImpl() {
      log_ = logger::log("SFV");
    }

    std::shared_ptr<shared_model::interface::Proposal>
    StatefulValidatorImpl::validate(
        const std::shared_ptr<shared_model::interface::Proposal> &proposal,
        ametsuchi::TemporaryWsv &temporaryWsv) {
      log_->info("transactions in proposal: {}",
                 proposal->transactions().size());
      auto checking_transaction = [this](const auto &tx, auto &queries) {
        return (queries.getAccount(tx.creator_account_id) |
                [&](const auto &account) {
                  // Check if tx creator has account and has quorum to execute
                  // transaction
                  return tx.signatures.size() >= account.quorum
                      ? queries.getSignatories(tx.creator_account_id)
                      : nonstd::nullopt;
                }
                |
                [&](const auto &signatories) {
                  auto model_signatories =
                      signatories
                      | boost::adaptors::transformed([](const auto &signatory) {
                          return shared_model::crypto::PublicKey(
                              signatory.to_string());
                        });
                  // Check if signatures in transaction are account signatory
                  return this->signaturesSubset(
                             shared_model::proto::from_old(tx).signatures(),
                             std::vector<shared_model::crypto::PublicKey>(
                                 model_signatories.begin(),
                                 model_signatories.end()))
                      ? nonstd::make_optional(model_signatories)
                      : nonstd::nullopt;
                })
            .has_value();
      };

      // Filter only valid transactions
      auto filter = [&temporaryWsv, checking_transaction](auto &acc,
                                                          const auto &tx) {
        std::unique_ptr<model::Transaction> old_tx(tx->makeOldModel());
        auto answer = temporaryWsv.apply(*old_tx, checking_transaction);
        if (answer) {
          acc.push_back(tx);
        }
        return acc;
      };

      auto &txs = proposal->transactions();
      decltype(txs) valid = {};

      auto valid_txs = std::accumulate(txs.begin(), txs.end(), valid, filter);

      // rework validation logic, so that this conversion is not needed
      auto valid_proto_txs =
          valid_txs
          | boost::adaptors::transformed([](const auto &polymorphic_tx) {
              return static_cast<const shared_model::proto::Transaction&>(
                  *polymorphic_tx.operator->());
            });
      auto validated_proposal = shared_model::proto::ProposalBuilder()
                                    .createdTime(proposal->created_time())
                                    .height(proposal->height())
                                    .transactions(valid_proto_txs)
                                    .createdTime(proposal->created_time())
                                    .build();

      log_->info("transactions in verified proposal: {}",
                 validated_proposal.transactions().size());
      return std::make_shared<decltype(validated_proposal)>(
          validated_proposal.getTransport());
    }

    bool StatefulValidatorImpl::signaturesSubset(
        const shared_model::interface::Transaction::SignatureSetType
            &signatures,
        const std::vector<shared_model::crypto::PublicKey> &public_keys) {
      // TODO 09/10/17 Lebedev: simplify the subset verification IR-510
      // #goodfirstissue
      std::unordered_set<std::string> txPubkeys;
      for (auto sign : signatures) {
        txPubkeys.insert(sign->publicKey().toString());
      }
      return std::all_of(public_keys.begin(),
                         public_keys.end(),
                         [&txPubkeys](const auto &public_key) {
                           return txPubkeys.find(public_key.toString())
                               != txPubkeys.end();
                         });
    }

  }  // namespace validation
}  // namespace iroha
