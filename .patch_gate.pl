#!/usr/bin/perl
# World field: survive a transient Atmo gate-off without dropping tiles; refeed on the way back; log both edges.
# Physics fetch: log answered/missing per batch so a region that never answers shows in the log.
use strict; use warnings;
my $n = 0;
sub patch { my ($f, @pairs) = @_; open(my $fh, '<', $f) or die $!; local $/; my $s = <$fh>; close $fh;
  my $crlf = ($s =~ /\r\n/) ? 1 : 0; $s =~ s/\r\n/\n/g;
  while (@pairs) { my $old = shift @pairs; my $new = shift @pairs; my $i = index($s, $old); die "anchor not found in $f: $old" if $i < 0; die "anchor not unique in $f: $old" if index($s, $old, $i + 1) >= 0; substr($s, $i, length($old)) = $new; $n++; }
  $s =~ s/\n/\r\n/g if $crlf;
  open($fh, '>', $f) or die $!; print $fh $s; close $fh; }

patch('indra/newview/ssworldfield.cpp',
  "    if (!enabled || !SSAtmoMagic::getInstance()->isEnabled())\n    {\n        if (!mTiles.empty() || mBuild.mActive) clear();\n        return;\n    }\n",
  "    // <SS:Nexii> The Atmo gate flickers: a death teleport or an altitude with no track resolves no environment for a while, and clearing the store on every dip threw away every tile and every consumer's overlay with it, then left the navmesh-fed tiles stale on the way back (their nav-dirty flag was down, so nothing refed them). The setting still clears; the Atmo gate only pauses, and the return edge invalidates every navmesh-fed tile and asks the navmesh for its region again. Both edges log. [interaction: navSettle, SSNavMesh::refeedRegion]\n" .
  "    if (!enabled)\n    {\n        if (!mTiles.empty() || mBuild.mActive) clear();\n        return;\n    }\n" .
  "    const bool gate_on = SSAtmoMagic::getInstance()->isEnabled();\n" .
  "    if (!gate_on)\n    {\n" .
  "        if (!mGateWasOff) LL_INFOS(\"SSWorldField\") << \"world field paused: Atmo Magic resolved no environment (\" << mTiles.size() << \" tiles kept)\" << LL_ENDL;\n" .
  "        mGateWasOff = true;\n" .
  "        mBuild.mActive = false;\n" .
  "        return;\n" .
  "    }\n" .
  "    if (mGateWasOff)\n    {\n" .
  "        mGateWasOff = false;\n" .
  "        LL_INFOS(\"SSWorldField\") << \"world field resumed: Atmo Magic environment back, refeeding \" << mTiles.size() << \" tiles\" << LL_ENDL;\n" .
  "        for (auto& entry : mTiles)\n" .
  "        {\n" .
  "            if (!entry.second.mNavSourced) continue;\n" .
  "            entry.second.mValid = false;\n" .
  "            entry.second.mNavDirty = true;\n" .
  "            entry.second.mNavLastFed = mNow;\n" .
  "            SSNavMesh::getInstance()->refeedRegion(entry.first);\n" .
  "        }\n" .
  "    }\n");

patch('indra/newview/ssworldfield.h',
  "        S32 mBandCount = 0;        // effective bands; bands [0, mBandCount) are live\n",
  "        S32 mBandCount = 0;        // effective bands; bands [0, mBandCount) are live\n");

patch('indra/newview/llviewerobjectlist.cpp',
  "    // Success, grab the resource cost and linked set costs\n    // for an object if one was returned\n    for (LLSD::array_iterator it = idList.beginArray(); it != idList.endArray(); ++it)\n    {\n        LLUUID objectId = it->asUUID();\n",
  "    // Success, grab the resource cost and linked set costs\n    // for an object if one was returned\n    S32 answered = 0, missing = 0;     // <SS:Nexii> per-batch tally, logged below: a region that never answers for its objects shows here\n    for (LLSD::array_iterator it = idList.beginArray(); it != idList.endArray(); ++it)\n    {\n        LLUUID objectId = it->asUUID();\n",
  "            gObjectList.updatePhysicsShapeType(objectId, shapeType);\n",
  "            gObjectList.updatePhysicsShapeType(objectId, shapeType);\n            ++answered;\n",
  "            // TODO*: Give user feedback about the missing data?\n            gObjectList.onPhysicsFlagsFetchFailure(objectId);\n        }\n    }\n}\n",
  "            // TODO*: Give user feedback about the missing data?\n            gObjectList.onPhysicsFlagsFetchFailure(objectId);\n            ++missing;\n        }\n    }\n    LL_INFOS(\"SSPhysFetch\") << \"physics shapes: \" << answered << \" answered, \" << missing << \" missing of \" << idList.size() << \" from \" << url << LL_ENDL;\n}\n");

print "applied $n replacements\n";
