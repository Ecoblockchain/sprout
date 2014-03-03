/**
 * @file handlers.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include <json/reader.h>

#include "handlers.h"
#include "log.h"
#include "regstore.h"
#include "ifchandler.h"

//LCOV_EXCL_START - don't want to actually run the handlers in the UT
void ChronosHandler::run()
{
  if (_req.method() != htp_method_POST)
  {
    _req.send_reply(405);
    delete this;
    return;
  }

  int rc = parse_response(_req.body());
  if (rc != 200)
  {
    LOG_DEBUG("Unable to parse response from Chronos");
    _req.send_reply(rc);
    delete this;
    return;
  }

  _req.send_reply(200);
  handle_response();
  delete this;
}
//LCOV_EXCL_STOP

void ChronosHandler::handle_response()
{
  RegStore::AoR* aor_data = set_aor_data(_cfg->_store, _aor_id, NULL, _cfg->_remote_store, true);

  if (aor_data != NULL)
  {
    // If we have a remote store, try to store this there too.  We don't worry
    // about failures in this case.
    if (_cfg->_remote_store != NULL)
    {
      RegStore::AoR* remote_aor_data = set_aor_data(_cfg->_remote_store, _aor_id, aor_data, NULL, false);
      delete remote_aor_data;
    }
  }

  delete aor_data;
}

RegStore::AoR* ChronosHandler::set_aor_data(RegStore* current_store,
                                            std::string aor_id,
                                            RegStore::AoR* previous_aor_data,
                                            RegStore* remote_store,
                                            bool is_primary)
{
  RegStore::AoR* aor_data = NULL;
  bool previous_aor_data_alloced = false;
  bool all_bindings_expired = false;

  do
  {
    // delete NULL is safe, so we can do this on every iteration.
    delete aor_data;

    // Find the current bindings for the AoR.
    aor_data = current_store->get_aor_data(aor_id);
    LOG_DEBUG("Retrieved AoR data %p", aor_data);

    if (aor_data == NULL)
    {
      // Failed to get data for the AoR because there is no connection
      // to the store.
      // LCOV_EXCL_START - local store (used in testing) never fails
      LOG_ERROR("Failed to get AoR binding for %s from store", aor_id.c_str());
      break;
      // LCOV_EXCL_STOP
    }

    // If we don't have any bindings, try the backup AoR and/or store.
    if (aor_data->bindings().empty())
    {
      if ((previous_aor_data == NULL) &&
          (remote_store != NULL))
      {
        previous_aor_data = remote_store->get_aor_data(aor_id);
        previous_aor_data_alloced = true;
      }

      if ((previous_aor_data != NULL) &&
          (!previous_aor_data->bindings().empty()))
      {
        //LCOV_EXCL_START
        for (RegStore::AoR::Bindings::const_iterator i = previous_aor_data->bindings().begin();
             i != previous_aor_data->bindings().end();
             ++i)
        {
          RegStore::AoR::Binding* src = i->second;
          RegStore::AoR::Binding* dst = aor_data->get_binding(i->first);
          *dst = *src;
        }

        for (RegStore::AoR::Subscriptions::const_iterator i = previous_aor_data->subscriptions().begin();
             i != previous_aor_data->subscriptions().end();
             ++i)
        {
          RegStore::AoR::Subscription* src = i->second;
          RegStore::AoR::Subscription* dst = aor_data->get_subscription(i->first);
          *dst = *src;
        }
        //LCOV_EXCL_STOP
      }
    }
  }
  while (!current_store->set_aor_data(aor_id, aor_data, is_primary, all_bindings_expired));

  if (is_primary && all_bindings_expired)
  {
    LOG_DEBUG("All bindings have expired based on a Chronos callback - triggering deregistration at the HSS");
    _cfg->_hss->update_registration_state(aor_id, "", HSSConnection::DEREG_TIMEOUT, 0);
  }

  // If we allocated the AoR, tidy up.
  if (previous_aor_data_alloced)
  {
    delete previous_aor_data;
  }

  return aor_data;
}

// Retrieve the aor and binding ID from the opaque data
int ChronosHandler::parse_response(std::string body)
{
  Json::Value json_body;
  std::string json_str = body;
  Json::Reader reader;
  bool parsingSuccessful = reader.parse(json_str.c_str(), json_body);

  if (!parsingSuccessful)
  {
    LOG_WARNING("Failed to read opaque data, %s",
                reader.getFormattedErrorMessages().c_str());
    return 400;
  }

  if ((json_body.isMember("aor_id")) &&
      ((json_body)["aor_id"].isString()))
  {
    _aor_id = json_body.get("aor_id", "").asString();
  }
  else
  {
    LOG_WARNING("AoR ID not available in JSON");
    return 400;
  }

  if ((json_body.isMember("binding_id")) &&
      ((json_body)["binding_id"].isString()))
  {
    _binding_id = json_body.get("binding_id", "").asString();
  }
  else
  {
    LOG_WARNING("Binding ID not available in JSON");
    return 400;
  }

  return 200;
}