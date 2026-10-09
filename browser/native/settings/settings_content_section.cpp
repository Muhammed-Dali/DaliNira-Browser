#include "settings_page.h"
#include "settings_ui_helpers.h"
#include "browser_profile_service.h"
#include "i18n/i18n.h"

using namespace dalinira::settings_ui;

QWidget *SettingsPage::createContentSection() {
  Section section = makeSection(I18n::text(QStringLiteral("settings.content.title")), I18n::text(QStringLiteral("settings.content.description")));

  auto *contentCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.content_card")));

  // Çerezleri engelle
  const auto getCookieSub = [this] {
    const auto pol = profileService_->cookiePolicy();
    if (pol == QStringLiteral("block_all")) return I18n::text(QStringLiteral("settings.privacy.cookies_block_all"));
    if (pol == QStringLiteral("allow_all")) return I18n::text(QStringLiteral("settings.privacy.cookies_allow_all"));
    return I18n::text(QStringLiteral("settings.privacy.cookies_block_third_party"));
  };
  InteractiveSettingRowResult cookieRow;
  cookieRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Cookie, I18n::text(QStringLiteral("settings.content.block_cookies")), getCookieSub(),
      [this](QLabel *subLabel) {
        const auto pol = profileService_->cookiePolicy();
        int idx = pol == QStringLiteral("block_all") ? 2 : (pol == QStringLiteral("allow_all") ? 1 : 0);
        showOptionDialog(
            this, I18n::text(QStringLiteral("settings.content.cookie_settings")), BrowserIcon::Cookie,
            I18n::text(QStringLiteral("settings.content.cookie_settings_desc")),
            {
                {I18n::text(QStringLiteral("settings.content.block_third_party_recommended")), I18n::text(QStringLiteral("settings.content.block_third_party_desc"))},
                {I18n::text(QStringLiteral("settings.content.allow_all_cookies")), I18n::text(QStringLiteral("settings.content.allow_all_cookies_desc"))},
                {I18n::text(QStringLiteral("settings.content.block_all_cookies")), I18n::text(QStringLiteral("settings.content.block_all_cookies_desc"))}
            },
            idx,
            [this, subLabel](int chosen) {
              QString newPol = chosen == 2 ? QStringLiteral("block_all") : (chosen == 1 ? QStringLiteral("allow_all") : QStringLiteral("third_party"));
              profileService_->setCookiePolicy(newPol);
              if (subLabel) {
                subLabel->setText(chosen == 2 ? I18n::text(QStringLiteral("settings.privacy.cookies_block_all")) : (chosen == 1 ? I18n::text(QStringLiteral("settings.privacy.cookies_allow_all")) : I18n::text(QStringLiteral("settings.privacy.cookies_block_third_party"))));
              }
            });
      });
  addRow(contentCard, cookieRow.frame);

  // JavaScript
  const auto getJsSub = [this] {
    return profileService_->isJavascriptEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.javascript_allowed"))
        : I18n::text(QStringLiteral("settings.site.javascript_deny"));
  };
  InteractiveSettingRowResult jsRow;
  jsRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Javascript, QStringLiteral("JavaScript"), getJsSub(),
      [this](QLabel *subLabel) {
        showOptionDialog(
            this, QStringLiteral("JavaScript"), BrowserIcon::Javascript,
            I18n::text(QStringLiteral("settings.content.javascript_desc")),
            {
                {I18n::text(QStringLiteral("settings.site.javascript_allow_recommended")), I18n::text(QStringLiteral("settings.content.javascript_allow_desc"))},
                {I18n::text(QStringLiteral("settings.site.javascript_deny")), I18n::text(QStringLiteral("settings.content.javascript_block_desc"))}
            },
            profileService_->isJavascriptEnabled() ? 0 : 1,
            [this, subLabel](int chosen) {
              profileService_->setJavascriptEnabled(chosen == 0);
              if (subLabel) {
                subLabel->setText(chosen == 0 ? I18n::text(QStringLiteral("settings.privacy.javascript_allowed")) : I18n::text(QStringLiteral("settings.site.javascript_deny")));
              }
            });
      });
  addRow(contentCard, jsRow.frame);

  // Resimler
  const auto getImgSub = [this] {
    return profileService_->isAutoLoadImagesEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.images_allowed"))
        : I18n::text(QStringLiteral("settings.site.images_deny"));
  };
  InteractiveSettingRowResult imgRow;
  imgRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Image, I18n::text(QStringLiteral("settings.site.images")), getImgSub(),
      [this](QLabel *subLabel) {
        showOptionDialog(
            this, I18n::text(QStringLiteral("settings.site.images")), BrowserIcon::Image,
            I18n::text(QStringLiteral("settings.content.images_desc")),
            {
                {I18n::text(QStringLiteral("settings.site.images_allow_recommended")), I18n::text(QStringLiteral("settings.content.images_allow_desc"))},
                {I18n::text(QStringLiteral("settings.site.images_deny")), I18n::text(QStringLiteral("settings.content.images_block_desc"))}
            },
            profileService_->isAutoLoadImagesEnabled() ? 0 : 1,
            [this, subLabel](int chosen) {
              profileService_->setAutoLoadImagesEnabled(chosen == 0);
              if (subLabel) {
                subLabel->setText(chosen == 0 ? I18n::text(QStringLiteral("settings.privacy.images_allowed")) : I18n::text(QStringLiteral("settings.site.images_deny")));
              }
            });
      });
  addRow(contentCard, imgRow.frame);

  // Pop-up ve yönlendirmeler
  const auto getPopupSub = [this] {
    return !profileService_->arePopupsAllowed()
        ? I18n::text(QStringLiteral("settings.privacy.popups_blocked"))
        : I18n::text(QStringLiteral("settings.privacy.popups_allowed"));
  };
  InteractiveSettingRowResult popupRow;
  popupRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Popup, I18n::text(QStringLiteral("settings.site.popups")), getPopupSub(),
      [this](QLabel *subLabel) {
        showOptionDialog(
            this, I18n::text(QStringLiteral("settings.site.popups")), BrowserIcon::Popup,
            I18n::text(QStringLiteral("settings.content.popups_desc")),
            {
                {I18n::text(QStringLiteral("settings.site.popups_deny_recommended")), I18n::text(QStringLiteral("settings.content.popups_block_desc"))},
                {I18n::text(QStringLiteral("settings.privacy.popups_allowed")), I18n::text(QStringLiteral("settings.content.popups_allow_desc"))}
            },
            !profileService_->arePopupsAllowed() ? 0 : 1,
            [this, subLabel](int chosen) {
              profileService_->setPopupsAllowed(chosen == 1);
              if (subLabel) {
                subLabel->setText(chosen == 0 ? I18n::text(QStringLiteral("settings.privacy.popups_blocked")) : I18n::text(QStringLiteral("settings.privacy.popups_allowed")));
              }
            });
      });
  addRow(contentCard, popupRow.frame);

  section.layout->addWidget(contentCard);
  section.layout->addStretch();
  return section.page;
}
