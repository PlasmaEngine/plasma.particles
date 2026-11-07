#pragma once

/// \brief The particle effect's panels in the engine shell.
///
/// The shell shows an effect's preview, properties and the panels its window declares (graph and timeline) by itself
/// (plEngineAssetDocumentContentProvider); this provider arranges them the way graph documents are: the graph on the
/// left, the preview above the properties on the right, the timeline under the preview. It also shows the report of a
/// conversion from plasma.particles 1.3 as a banner below the document's toolbar (plDocumentNotices).
namespace plParticleEffectShellPanels
{
  void Register();
  void Unregister();
} // namespace plParticleEffectShellPanels
