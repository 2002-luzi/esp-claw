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

type TtsForm = {
  tts_provider: string;
  tts_api_key: string;
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
      tts_api_key: config.tts_api_key ?? '',
      tts_base_url: config.tts_base_url ?? '',
      tts_model: config.tts_model ?? '',
      tts_voice: config.tts_voice ?? '',
      tts_timeout_ms: config.tts_timeout_ms ?? '120000',
    }),
    fromForm: (form) => ({
      tts_provider: form.tts_provider.trim(),
      tts_api_key: form.tts_api_key.trim(),
      tts_base_url: form.tts_base_url.trim(),
      tts_model: form.tts_model.trim(),
      tts_voice: form.tts_voice.trim(),
      tts_timeout_ms: form.tts_timeout_ms.trim(),
    }),
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
              value={tab.form.tts_api_key}
              onInput={(event) => tab.setForm('tts_api_key', event.currentTarget.value)}
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
