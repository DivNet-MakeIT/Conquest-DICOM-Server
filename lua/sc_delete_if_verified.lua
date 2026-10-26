-- EXAMPLE ONLY - not enabled anywhere. Copy and adapt it for a routing setup.
--
-- Delete a study locally only after the archive has confirmed, in this very call, that it holds
-- every local object of the study (outgoing storage commitment, sc_verify_now:). The stored
-- "study confirmed" state (sc_confirmed:, scconfirmed, the OutgoingSCStudyConfirmed hook) is
-- advisory and is NOT a basis for deleting.
--
-- Needs OutgoingStorageCommitment = 1 in dicom.ini and SC (or TSC) for the archive in acrnema.map.
-- Call it from a script that runs in the server, e.g. a [lua] nightly or background script, with
-- the patient ID, the StudyInstanceUID and the AE of the archive:
--   dofile('lua/sc_delete_if_verified.lua')
--   sc_delete_if_verified('PATID', '1.2.3.4', 'ARCHIVE')

function sc_delete_if_verified(patid, study, ae, timeout)
  -- verify immediately before deleting; delete only on true
  local ok, why = scverifynow(study, ae, timeout or 120)
  if ok == true then
    print('sc_delete_if_verified: archive ' .. ae .. ' committed study ' .. study .. ' now: deleting it locally')
    servercommand('deletestudy:' .. patid .. ':' .. study)
    return true
  end
  print('sc_delete_if_verified: study ' .. study .. ' NOT deleted (' .. tostring(why) .. ')')
  return false
end
