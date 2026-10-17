#include "calendar/sync_failure.h"
#include "net/http_client.h"

#include <print>

namespace {

  using calendar::SyncFailure;
  using calendar::SyncFailureStreak;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_sync_failure_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  bool ok = true;

  // A failure a retry won't fix is shown at once, and only notified once.
  SyncFailureStreak streak;
  ok =
      expect(calendar::recordSyncResult(streak, SyncFailure::Authentication), "authentication failure not shown") && ok;
  ok = expect(streak.shown && streak.failure == SyncFailure::Authentication, "shown failure is not authentication")
      && ok;
  ok = expect(!calendar::recordSyncResult(streak, SyncFailure::Authentication), "the same failure changed again") && ok;
  // Another kind while shown is a change.
  ok = expect(calendar::recordSyncResult(streak, SyncFailure::NotFound), "a new failure kind was not a change") && ok;
  // A success clears it.
  ok = expect(calendar::recordSyncResult(streak, SyncFailure::None), "a success did not clear the failure") && ok;
  ok = expect(streak == SyncFailureStreak{}, "a success left a streak") && ok;

  // A transient failure is shown only the second time in a row, of any transient kind.
  ok = expect(!calendar::recordSyncResult(streak, SyncFailure::Network), "a single network failure was shown") && ok;
  ok = expect(calendar::recordSyncResult(streak, SyncFailure::Timeout), "a second transient failure was not shown")
      && ok;
  ok = expect(streak.shown && streak.failure == SyncFailure::Timeout, "the shown failure is not the latest") && ok;

  // One transient failure between successes is never shown.
  streak = {};
  (void)calendar::recordSyncResult(streak, SyncFailure::Server);
  ok = expect(!calendar::recordSyncResult(streak, SyncFailure::None), "a hidden failure's end was a change") && ok;
  ok = expect(!calendar::recordSyncResult(streak, SyncFailure::Server), "a transient failure was shown after a success")
      && ok;

  // A locked keyring stays locked until someone unlocks it: shown at once.
  streak = {};
  ok = expect(calendar::recordSyncResult(streak, SyncFailure::CredentialsLocked), "a locked keyring was not shown")
      && ok;

  // A cancelled sync leaves the streak as it was.
  const SyncFailureStreak before = streak;
  ok = expect(!calendar::recordSyncResult(streak, SyncFailure::Cancelled), "a cancelled sync was a change") && ok;
  ok = expect(streak == before, "a cancelled sync changed the streak") && ok;

  // HTTP responses.
  ok = expect(
           calendar::syncFailureFromResponse(HttpResponse{.transportOk = true, .status = 403})
               == SyncFailure::Authentication,
           "HTTP 403 is not an authentication failure"
       )
      && ok;
  ok = expect(
           calendar::syncFailureFromResponse(HttpResponse{.transportOk = true, .status = 405}) == SyncFailure::NotFound,
           "HTTP 405 is not a missing calendar"
       )
      && ok;
  ok = expect(
           calendar::syncFailureFromResponse(HttpResponse{.transportOk = true, .status = 302})
               == SyncFailure::InvalidResponse,
           "an unexpected status is not an invalid response"
       )
      && ok;

  // A Google account is reconnected rather than given a password.
  ok = expect(
           calendar::syncFailureMessageKey(SyncFailure::Credentials, true)
                   == calendar::syncFailureMessageKey(SyncFailure::Authentication, true)
               && calendar::syncFailureMessageKey(SyncFailure::Credentials, true)
                   != calendar::syncFailureMessageKey(SyncFailure::Credentials),
           "a Google account's credentials have no message of their own"
       )
      && ok;

  // Every shown failure has a message.
  for (const SyncFailure failure :
       {SyncFailure::Configuration, SyncFailure::Credentials, SyncFailure::CredentialsLocked,
        SyncFailure::Authentication, SyncFailure::NotFound, SyncFailure::NoCalendars, SyncFailure::Timeout,
        SyncFailure::Network, SyncFailure::Server, SyncFailure::InvalidResponse}) {
    ok = expect(!calendar::syncFailureMessageKey(failure).empty(), "a failure has no message") && ok;
  }
  return ok ? 0 : 1;
}
