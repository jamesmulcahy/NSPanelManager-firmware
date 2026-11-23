#pragma once
#include <cstddef>
#include <protobuf_nspanel_entity.pb-c.h>
#include <string>
#include <vector>

/*
 * Lets the user pick a source for a media player. Drawn on the items4 page when there are up to four
 * sources, otherwise on items8, paging through it when there are more than eight.
 * Opened from the media player page by pressing the album art. Pressing a source, or the back
 * button, returns to that media player page.
 *
 * A media player fed by another player (an audio matrix zone playing from a Sonos speaker, say)
 * has two lists, as in the manager's protobuf: its own sources (the zone's inputs) and those of the
 * source player feeding it (the speaker's favorites). The media player page's source switch picks
 * which one is shown, the same way it picks what the volume buttons control.
 */
class MediaSourcePage {
public:
  /**
   * Show the sources of the media player whose state is published on media_player_state_topic.
   * @param source_player: Show the sources of the player feeding it rather than its own.
   * @param sources: The list to show, from the media player's state. Copied, so it need not outlive the call.
   */
  static void show(std::string media_player_state_topic, bool source_player, const NSPanelEntityState__MediaPlayer__SourceSelection *sources);

  /**
   * Unshow the source page.
   */
  static void unshow();

private:
  /**
   * An EntitiesView action (as int, keeping LVGL out of this header) on LvglUi's task.
   */
  static void _lvgl_action(int action, int32_t slot);

  /**
   * Draw the sources on the current page.
   */
  static void _update_display();

  /**
   * Select the source with the given index and return to the media player.
   */
  static void _select_source(size_t source_index);

  /**
   * Return to the media player page the source page was opened from.
   */
  static void _return_to_media_player();

  // The media player's state topic, to return to its page.
  static inline std::string _media_player_state_topic;

  // Whether the sources shown are those of the source player rather than the media player's own.
  static inline bool _showing_source_player = false;

  // The sources to pick from, and the one currently selected, as in the state when the page opened.
  // The current source is often empty
  // for a source player: Sonos only reports one for Line-in and TV, not for a playing favorite.
  static inline std::vector<std::string> _sources;
  static inline std::string _current_source;

  // The items page used, 4, 8 or 12, or 0 when the source page is not showing.
  static inline uint8_t _page_type = 0;

  // Index of the source in the first slot, when paging through more sources than fit on one page.
  static inline size_t _first_source_index = 0;
};
