import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import type { AppConfig, LuaModuleItem } from '../api/client';
import { appCapabilities, appConfig, appLuaModules } from '../state/config';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';
import { ConfigTable } from '../components/ui/ConfigTable';
import { SelectInput, TextInput } from '../components/ui/FormField';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

const SENTINEL_NONE = '__none__';

type SkillForm = {
  enabled: string[];
  tts_provider: string;
  tts_api_key: string;
  tts_base_url: string;
  tts_model: string;
  tts_voice: string;
  tts_timeout_ms: string;
};

function parseModules(serialized: string, items: LuaModuleItem[], fallbackAll: boolean): string[] {
  const raw = (serialized || '').trim();
  if (!raw) {
    return fallbackAll ? items.map((item) => item.module_id) : [];
  }
  if (raw === SENTINEL_NONE || raw === 'none') return [];
  return raw
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token && token !== SENTINEL_NONE && token !== 'none');
}

function serializeModules(selected: string[], items: LuaModuleItem[]): string {
  if (items.length === 0) return '';
  const set = new Set(selected);
  if (set.size === items.length) return '';
  if (set.size === 0) return SENTINEL_NONE;
  return Array.from(set).join(',');
}

function hasModule(items: LuaModuleItem[], moduleId: string): boolean {
  return items.some((item) => item.module_id === moduleId);
}

function isCapLuaEnabled(): boolean {
  const raw = (appConfig().enabled_cap_groups || '').trim();
  if (!raw) {
    return appCapabilities().some((item) => item.group_id === 'cap_lua');
  }
  if (raw === SENTINEL_NONE || raw === 'none') return false;
  return raw
    .split(',')
    .map((token) => token.trim())
    .includes('cap_lua');
}

export const SkillsPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<SkillForm>({
    tab: 'skills',
    groups: ['skills', 'capabilities', 'tts'],
    toForm: (config: Partial<AppConfig>) => ({
      enabled: parseModules(config.enabled_lua_modules ?? '', appLuaModules(), true),
      tts_provider: config.tts_provider ?? 'xiao_mimo',
      tts_api_key: config.tts_api_key ?? '',
      tts_base_url: config.tts_base_url ?? '',
      tts_model: config.tts_model ?? '',
      tts_voice: config.tts_voice ?? '',
      tts_timeout_ms: config.tts_timeout_ms ?? '120000',
    }),
    fromForm: (form) => {
      const modules = appLuaModules();
      const patch: Partial<AppConfig> = {
        enabled_lua_modules: serializeModules(form.enabled, modules),
      };
      if (hasModule(modules, 'tts')) {
        patch.tts_provider = form.tts_provider.trim();
        patch.tts_api_key = form.tts_api_key.trim();
        patch.tts_base_url = form.tts_base_url.trim();
        patch.tts_model = form.tts_model.trim();
        patch.tts_voice = form.tts_voice.trim();
        patch.tts_timeout_ms = form.tts_timeout_ms.trim();
      }
      return patch;
    },
    isDirty: (form, baseline) => {
      const a = new Set(form.enabled);
      const b = new Set(baseline.enabled);
      if (a.size !== b.size) return true;
      for (const v of a) if (!b.has(v)) return true;
      return (
        form.tts_provider !== baseline.tts_provider ||
        form.tts_api_key !== baseline.tts_api_key ||
        form.tts_base_url !== baseline.tts_base_url ||
        form.tts_model !== baseline.tts_model ||
        form.tts_voice !== baseline.tts_voice ||
        form.tts_timeout_ms !== baseline.tts_timeout_ms
      );
    },
  });

  const [search, setSearch] = createSignal('');
  const [confirmOpen, setConfirmOpen] = createSignal(false);
  const enabledSet = createMemo(() => new Set(tab.form.enabled));
  const modulesDirty = createMemo(() => {
    const current = new Set(tab.form.enabled);
    const baseline = new Set(parseModules(appConfig().enabled_lua_modules ?? '', appLuaModules(), true));
    if (current.size !== baseline.size) return true;
    for (const item of current) {
      if (!baseline.has(item)) return true;
    }
    return false;
  });

  const filtered = createMemo(() => {
    const keyword = search().trim().toLowerCase();
    return appLuaModules().filter((item) => {
      if (!keyword) return true;
      return (
        item.module_id.toLowerCase().includes(keyword) ||
        (item.display_name ?? '').toLowerCase().includes(keyword)
      );
    });
  });

  const toggle = (id: string, checked: boolean) => {
    const set = new Set(tab.form.enabled);
    if (checked) set.add(id);
    else set.delete(id);
    tab.setForm('enabled', Array.from(set));
  };

  const selectAll = () => {
    tab.setForm(
      'enabled',
      appLuaModules().map((item) => item.module_id),
    );
  };
  const clearAll = () => {
    tab.setForm('enabled', []);
  };

  const handleSave = async () => {
    const needsRestart = modulesDirty();
    await tab.save();
    if (needsRestart) {
      setConfirmOpen(true);
    }
  };

  const summary = () =>
    tf('luaModulesSummary', {
      enabled: tab.form.enabled.length,
      total: appLuaModules().length,
    });

  return (
    <TabShell>
      <PageHeader
        title={t('sectionLuaModules') as string}
        description={t('luaModulesDescription') as string}
      />
      <div class="px-5 py-4 flex flex-col gap-3 border-b border-[var(--color-border-subtle)]">
        <Show when={!isCapLuaEnabled()}>
          <Banner kind="info" message={t('luaModulesCapabilityRequired') as string} />
        </Show>
        <div class="flex flex-wrap gap-2 items-center">
          <input
            type="search"
            placeholder={t('luaModulesSearchPlaceholder') as string}
            class="flex-1 min-w-[160px] max-w-xs"
            value={search()}
            onInput={(event) => setSearch(event.currentTarget.value)}
          />
          <Button size="sm" variant="secondary" onClick={selectAll} disabled={!isCapLuaEnabled()}>
            {t('luaModulesSelectAll')}
          </Button>
          <Button size="sm" variant="secondary" onClick={clearAll} disabled={!isCapLuaEnabled()}>
            {t('luaModulesClearAll')}
          </Button>
        </div>
        <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">{summary()}</p>
      </div>

      <div class="p-5">
        <Show when={appLuaModules().length === 0}>
          <Banner kind="info" message={t('luaModulesLoading') as string} />
        </Show>
        <Show when={appLuaModules().length > 0 && filtered().length === 0}>
          <Banner kind="info" message={t('luaModulesNoResult') as string} />
        </Show>
        <ConfigTable
          columns={[
            { label: t('luaModulesNameCol') as string },
            { label: t('idCol') as string },
            { label: t('capabilityEnabled') as string, class: 'w-24 text-center' },
          ]}
        >
          <For each={filtered()}>
            {(item) => (
              <>
                <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                  <td class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)]">
                    <div class="flex flex-col gap-0.5">
                      <span class="font-semibold">{item.display_name || item.module_id}</span>
                    </div>
                  </td>
                  <td class="px-4 py-2.5 font-mono text-[0.78rem] text-[var(--color-text-muted)] break-all">
                    {item.module_id}
                  </td>
                  <td class="px-4 py-2.5 text-center">
                    <Switch
                      checked={enabledSet().has(item.module_id)}
                      disabled={!isCapLuaEnabled()}
                      onChange={(checked) => toggle(item.module_id, checked)}
                    />
                  </td>
                </tr>
                <Show when={item.module_id === 'tts' && enabledSet().has('tts')}>
                  <tr class="border-t border-[var(--color-border-subtle)] bg-[var(--color-bg-card)]/35">
                    <td colspan={3} class="px-4 py-4">
                      <div class="flex flex-col gap-4">
                        <div class="flex flex-col gap-1">
                          <h3 class="m-0 text-[0.86rem] font-semibold text-[var(--color-text-primary)]">
                            {t('sectionTtsProvider')}
                          </h3>
                          <p class="text-[0.76rem] text-[var(--color-text-muted)] m-0">
                            {t('ttsProviderNote')}
                          </p>
                        </div>
                        <div class="grid gap-3 sm:grid-cols-2">
                          <SelectInput
                            label={t('ttsProvider')}
                            value={tab.form.tts_provider}
                            onInput={(event) =>
                              tab.setForm('tts_provider', event.currentTarget.value)
                            }
                          >
                            <option value="xiao_mimo">{t('ttsProviderXiaoMimo') as string}</option>
                          </SelectInput>
                          <TextInput
                            type="password"
                            label={t('ttsApiKey')}
                            value={tab.form.tts_api_key}
                            onInput={(event) =>
                              tab.setForm('tts_api_key', event.currentTarget.value)
                            }
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
                          <TextInput
                            type="url"
                            label={t('ttsBaseUrl')}
                            placeholder={t('ttsBaseUrlPlaceholder') as string}
                            value={tab.form.tts_base_url}
                            onInput={(event) =>
                              tab.setForm('tts_base_url', event.currentTarget.value)
                            }
                          />
                          <TextInput
                            type="number"
                            min="1"
                            label={t('ttsTimeout')}
                            placeholder={t('ttsTimeoutPlaceholder') as string}
                            value={tab.form.tts_timeout_ms}
                            onInput={(event) =>
                              tab.setForm('tts_timeout_ms', event.currentTarget.value)
                            }
                          />
                        </div>
                      </div>
                    </td>
                  </tr>
                </Show>
              </>
            )}
          </For>
        </ConfigTable>
      </div>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={modulesDirty() ? (t('restartHint') as string) : undefined}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};
