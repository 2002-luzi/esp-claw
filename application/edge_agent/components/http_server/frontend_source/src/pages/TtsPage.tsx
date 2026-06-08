import { Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { SelectInput, TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { Switch } from '../components/ui/Switch';

type TtsForm = {
  tts_provider: string;
  tts_api_key_saved: boolean;
  tts_api_key_new: string;
  tts_api_key_clear: boolean;
  tts_base_url: string;
  tts_model: string;
  tts_voice: string;
  tts_timeout_ms: string;
};

export const TtsPage: Component = () => {
  const tab = createConfigTab<TtsForm>({
    tab: 'tts',
    groups: ['tts'],
    toForm: (config: Partial<AppConfig>) => ({
      tts_provider: config.tts_provider ?? 'xiao_mimo',
      tts_api_key_saved: !!config.tts_api_key,
      tts_api_key_new: '',
      tts_api_key_clear: false,
      tts_base_url: config.tts_base_url ?? '',
      tts_model: config.tts_model ?? '',
      tts_voice: config.tts_voice ?? '',
      tts_timeout_ms: config.tts_timeout_ms ?? '120000',
    }),
    fromForm: (form) => {
      const patch: Partial<AppConfig> = {
        tts_provider: form.tts_provider.trim(),
        tts_base_url: form.tts_base_url.trim(),
        tts_model: form.tts_model.trim(),
        tts_voice: form.tts_voice.trim(),
        tts_timeout_ms: form.tts_timeout_ms.trim(),
      };
      const apiKey = form.tts_api_key_new.trim();
      if (apiKey) {
        patch.tts_api_key = apiKey;
      } else if (form.tts_api_key_clear) {
        patch.tts_api_key = '';
      }
      return patch;
    },
  });
  return (
    <TabShell>
      <PageHeader title={t('navTts') as string} description={t('sectionTts') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionTtsProvider') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <SelectInput
              label={t('ttsProvider')}
              value={tab.form.tts_provider}
              onInput={(event) => tab.setForm('tts_provider', event.currentTarget.value)}
            >
              <option value="xiao_mimo">{t('ttsProviderXiaoMimo') as string}</option>
            </SelectInput>
            <TextInput
              type="password"
              label={t('ttsApiKey')}
              placeholder={t('ttsApiKeyPlaceholder') as string}
              hint={
                <span class="inline-flex flex-wrap items-center gap-x-3 gap-y-1">
                  <span>{t(tab.form.tts_api_key_saved ? 'ttsApiKeySavedHint' : 'ttsApiKeyEmptyHint')}</span>
                  <Show when={tab.form.tts_api_key_saved}>
                    <Switch
                      checked={tab.form.tts_api_key_clear}
                      label={t('ttsApiKeyClear') as string}
                      labelClass="text-[var(--color-text-secondary)]"
                      class="text-[0.72rem]"
                      onChange={(checked) => tab.setForm('tts_api_key_clear', checked)}
                    />
                  </Show>
                </span>
              }
              value={tab.form.tts_api_key_new}
              onInput={(event) => {
                tab.setForm('tts_api_key_new', event.currentTarget.value);
                if (event.currentTarget.value.trim()) {
                  tab.setForm('tts_api_key_clear', false);
                }
              }}
            />
            <TextInput
              label={t('ttsModel')}
              placeholder={t('ttsModelPlaceholder') as string}
              value={tab.form.tts_model}
              onInput={(event) => tab.setForm('tts_model', event.currentTarget.value)}
            />
            <TextInput
              label={t('ttsVoice')}
              placeholder={t('ttsVoicePlaceholder') as string}
              value={tab.form.tts_voice}
              onInput={(event) => tab.setForm('tts_voice', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">
            {t('ttsProviderNote')}
          </p>
        </StaticConfigBlock>
        <CollapsibleConfigBlock title={t('ttsAdvanced') as string} defaultOpen={false}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              type="url"
              label={t('ttsBaseUrl')}
              placeholder={t('ttsBaseUrlPlaceholder') as string}
              value={tab.form.tts_base_url}
              onInput={(event) => tab.setForm('tts_base_url', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              min="1"
              label={t('ttsTimeout')}
              placeholder={t('ttsTimeoutPlaceholder') as string}
              value={tab.form.tts_timeout_ms}
              onInput={(event) => tab.setForm('tts_timeout_ms', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => tab.save().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
