#include "calendar/sync_failure.h"

#include "net/http_client.h"

namespace calendar {

  SyncFailure syncFailureFromResponse(const HttpResponse& response) {
    if (!response.transportOk) {
      return response.timedOut ? SyncFailure::Timeout : SyncFailure::Network;
    }
    if (response.status == 401 || response.status == 403) {
      return SyncFailure::Authentication;
    }
    // 405: the address answers, but not as a CalDAV/WebDAV resource.
    if (response.status == 404 || response.status == 405 || response.status == 410) {
      return SyncFailure::NotFound;
    }
    if (response.status >= 500 && response.status < 600) {
      return SyncFailure::Server;
    }
    return SyncFailure::InvalidResponse;
  }

  bool recordSyncResult(SyncFailureStreak& streak, SyncFailure result) {
    if (result == SyncFailure::Cancelled) {
      return false;
    }
    const SyncFailureStreak previous = streak;
    if (result == SyncFailure::None) {
      streak = {};
    } else {
      ++streak.consecutiveFailures;
      streak.failure = result;
      streak.shown = !transientSyncFailure(result) || streak.consecutiveFailures >= 2;
    }
    return streak.shown != previous.shown || (streak.shown && streak.failure != previous.failure);
  }

  std::string_view syncFailureMessageKey(SyncFailure failure, bool google) {
    if (google && (failure == SyncFailure::Authentication || failure == SyncFailure::Credentials)) {
      return "notifications.internal.calendar-sync-failure-google-authorization";
    }
    switch (failure) {
    case SyncFailure::None:
    case SyncFailure::Cancelled:
      return {};
    case SyncFailure::Configuration:
      return "notifications.internal.calendar-sync-failure-configuration";
    case SyncFailure::Credentials:
      return "notifications.internal.calendar-sync-failure-credentials";
    case SyncFailure::CredentialsLocked:
      return "notifications.internal.calendar-sync-failure-credentials-locked";
    case SyncFailure::Authentication:
      return "notifications.internal.calendar-sync-failure-authentication";
    case SyncFailure::NotFound:
      return "notifications.internal.calendar-sync-failure-not-found";
    case SyncFailure::NoCalendars:
      return "notifications.internal.calendar-sync-failure-no-calendars";
    case SyncFailure::Timeout:
      return "notifications.internal.calendar-sync-failure-timeout";
    case SyncFailure::Network:
      return "notifications.internal.calendar-sync-failure-network";
    case SyncFailure::Server:
      return "notifications.internal.calendar-sync-failure-server";
    case SyncFailure::InvalidResponse:
      return "notifications.internal.calendar-sync-failure-invalid-response";
    }
    return {};
  }

} // namespace calendar
