#pragma once

#include <cstdint>
#include <string_view>

struct HttpResponse;

namespace calendar {

  // Why an account's sync failed, reported by every fetch path so the failure can be shown to the user
  // (a notification, the account's row in Settings) rather than only logged.
  enum class SyncFailure : std::uint8_t {
    None,
    Cancelled,         // the sync was abandoned (shutdown, a superseded request); never shown
    Configuration,     // a required setting is missing (server_url, username, path), or the type is unknown
    Credentials,       // no password is stored, or the password file can't be read
    CredentialsLocked, // the keyring is locked or Secret Service is unavailable
    Authentication,    // the server rejected the credentials (HTTP 401/403, a revoked Google grant)
    NotFound,          // nothing at the configured address: HTTP 404/405/410, a discovery step without an answer, a
                       // missing vdir directory
    NoCalendars,       // calendars were looked for, and none that hold events was found or selected
    Timeout,           // the transfer didn't finish in time
    Network,           // the server couldn't be reached (DNS, connection, TLS)
    Server,            // HTTP 5xx
    InvalidResponse,   // another HTTP status, or data that couldn't be parsed
  };

  // The failure of an HTTP response that wasn't the expected success.
  [[nodiscard]] SyncFailure syncFailureFromResponse(const HttpResponse& response);

  // Failures that a later attempt may not repeat (the network not up yet after login or resume, a busy
  // server, a keyring unlocked a moment later): shown only once they happen twice in a row.
  [[nodiscard]] constexpr bool transientSyncFailure(SyncFailure failure) {
    return failure == SyncFailure::Timeout
        || failure == SyncFailure::Network
        || failure == SyncFailure::Server
        || failure == SyncFailure::CredentialsLocked;
  }

  // An account's run of failed syncs, and whether its failure is shown (a notification, its Settings row): once
  // it isn't transient, or once it happened twice in a row. Failures of any kind count toward the run.
  struct SyncFailureStreak {
    SyncFailure failure = SyncFailure::None;
    int consecutiveFailures = 0;
    bool shown = false;

    bool operator==(const SyncFailureStreak&) const = default;
  };

  // Adds a sync's result to the streak (a success ends it; SyncFailure::Cancelled leaves it). Returns true
  // when what is shown changed: a failure starts or stops being shown, or the shown one becomes another.
  bool recordSyncResult(SyncFailureStreak& streak, SyncFailure result);

  // The translation key of the failure's explanation (under notifications.internal).
  [[nodiscard]] std::string_view syncFailureMessageKey(SyncFailure failure);

} // namespace calendar
