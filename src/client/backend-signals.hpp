//  OpenVPN 3 Linux client -- Next generation OpenVPN client
//
//  SPDX-License-Identifier: AGPL-3.0-only
//
//  Copyright (C)  OpenVPN Inc <sales@openvpn.net>
//  Copyright (C)  David Sommerseth <davids@openvpn.net>
//

/**
 * @file   backend-signals.hpp
 *
 * @brief  Helper class for Log, StatusChange and AttentionRequired
 *         sending signals
 */

#pragma once

#include <memory>
#include <sstream>
#include <thread>
#include <gdbuspp/signals/group.hpp>
#include <gdbuspp/signals/signal.hpp>
#include <gdbuspp/credentials/query.hpp>

#include "dbus/constants.hpp"
#include "dbus/signals/attention-required.hpp"
#include "dbus/signals/statuschange.hpp"
#include "log/dbus-log.hpp"
#include "log/logwriter.hpp"
#include "events/attention-req.hpp"
#include "events/status.hpp"


namespace Backend::Signals {

/**
 *  Helper class to send the RegistrationRequest signal to
 *  the session manager
 */
class RegistrationRequest : public DBus::Signals::Signal
{
  public:
    using Ptr = std::shared_ptr<RegistrationRequest>;

    RegistrationRequest(DBus::Signals::Emit::Ptr emitter)
        : DBus::Signals::Signal(emitter, "RegistrationRequest")
    {
        SetArguments({{"busname", glib2::DataType::DBus<std::string>()},
                      {"token", glib2::DataType::DBus<std::string>()},
                      {"pid", glib2::DataType::DBus<pid_t>()}});
    }

    bool Send(const std::string &busname,
              const std::string &token,
              const pid_t pid)
    {
        GVariantBuilder *b = glib2::Builder::Create("(ssi)");
        glib2::Builder::Add(b, busname);
        glib2::Builder::Add(b, token);
        glib2::Builder::Add(b, pid);

        try
        {
            return EmitSignal(glib2::Builder::Finish(b));
        }
        catch (const DBus::Signals::Exception &ex)
        {
            std::cerr << "RegistrationRequest::Send() EXCEPTION:"
                      << ex.what() << std::endl;
        }
        return false;
    }
};
} // namespace Client::Signals


class BackendSignals : public LogSender
{
  public:
    using Ptr = std::shared_ptr<BackendSignals>;

    BackendSignals(DBus::Connection::Ptr conn,
                   LogGroup lgroup,
                   std::string session_token,
                   LogWriter *logwr)
        : LogSender(conn,
                    lgroup,
                    Constants::GenPath("backends/session"),
                    Constants::GenInterface("backends"),
                    true,
                    logwr),
          session_token(session_token)
    {
        SetLogLevel(default_log_level);

        // Register the AttentionRequired and StatusChange signals
        // The LogSender extends DBus::Signals::Group, so we reuse the
        // access to the CreateSignal() method this way.
        sig_attreq = CreateSignal<::Signals::AttentionRequired>();
        sig_statuschg = CreateSignal<::Signals::StatusChange>();

        // Default targets for D-Bus signals are the
        // Session Manager (net.openvpn.v3.sessions) and the
        // Log service (net.openvpn.v3.log).
        auto creds = DBus::Credentials::Query::Create(conn);
        auto sessmgr_busn = creds->GetUniqueBusName(Constants::GenServiceName("sessions"));
        AddTarget(sessmgr_busn);
        AddTarget(creds->GetUniqueBusName(Constants::GenServiceName("log")));

        // Prepare the RegistrationRequest signal; this is only to be sent
        // to the Session Manager (net.openvpn.v3.sessions).  A dedicated signal
        // group is created for this, with only the session manager as the
        // recipient.  This Signal Group is used in RegistrationRequest()
        GroupCreate("sessionmgr");
        GroupAddTarget("sessionmgr", sessmgr_busn);
        sig_regreq = GroupCreateSignal<Backend::Signals::RegistrationRequest>("sessionmgr");
    }

    [[nodiscard]] static BackendSignals::Ptr Create(DBus::Connection::Ptr conn,
                                                    LogGroup lgroup,
                                                    std::string session_token,
                                                    LogWriter *logwr)
    {
        return BackendSignals::Ptr(new BackendSignals(conn,
                                                      lgroup,
                                                      session_token,
                                                      logwr));
    }

    void RegistrationRequest(const std::string &busname,
                             const std::string &token,
                             const pid_t pid)
    {
        sig_regreq->Send(busname, token, pid);
    }


    void StatusChange(const Events::Status &statusev)
    {
        sig_statuschg->Send(statusev);
    }


    void StatusChange(const StatusMajor maj, const StatusMinor min, const std::string &msg = "")
    {
        sig_statuschg->Send(Events::Status(maj, min, msg));
    }


    void Log(const Events::Log &logev,
             bool duplicate_check = false,
             const std::string &target = "") final
    {
        Events::Log l(logev, session_token);
        LogSender::Log(l, duplicate_check, logger_busname);
    }


    /**
     * Sends a FATAL log messages and kills itself
     *
     * @param Log message to send to the log subscribers
     */
    void LogFATAL(const std::string &msg) override
    {
        Log(Events::Log(log_group, LogCategory::FATAL, msg));
        // This is essentially a glib2 hack, to allow on going signals to
        // be properly sent before we shut down.
        delayed_shutdown.reset(
            new std::thread([]()
                            {
                                sleep(3);
                                kill(getpid(), SIGHUP);
                            }));
    }


    /**
     * Sends an AttentionRequired signal, which tells a front-end that this
     * VPN backend client needs some input or feedback.
     *
     * @param att_type   ClientAttentionType of the attention required
     * @param att_group  ClientAttentionGroup of the attention required
     * @param msg        Simple string message describing what is needed.
     */
    void AttentionReq(const ClientAttentionType att_type,
                      const ClientAttentionGroup att_group,
                      std::string msg)
    {
        sig_attreq->Send(att_type, att_group, msg);
    }


    /**
     *  Retrieve the last status message processed
     *
     * @return  Returns a GVariant object containing a key/value
     *          dictionary of the last signal sent
     */
    GVariant *GetLastStatusChange() const
    {
        return sig_statuschg->LastStatusChange();
    }


  private:
    const uint32_t default_log_level = 6; // LogCategory::DEBUG
    std::string session_token = {};
    std::string sessionmgr_busname = {};
    std::string logger_busname = {};
    ::Signals::AttentionRequired::Ptr sig_attreq = nullptr;
    ::Signals::StatusChange::Ptr sig_statuschg = nullptr;
    Backend::Signals::RegistrationRequest::Ptr sig_regreq = nullptr;
    std::unique_ptr<std::thread> delayed_shutdown;
};
