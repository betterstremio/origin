# Previews

Rest on a card and a short clip plays, about a minute long, with sound. Clips and trailers come from IMDb, and a title with no clip plays its trailer. They ask your stream addons for nothing, so they work with any setup. Settings > For You > Preview on focus chooses what plays, or turns previews off.

## The title itself

Preview on focus also offers The title itself: the film, past its opening credits, silent, for 30 seconds. **Only use it with a self-hosted AIOStreams or another provider with no public rate limit.** The app asks you to confirm before turning it on. If an addon reports that its rate limit is reached, previews use IMDb for the next 30 minutes. It asks your addons for links as soon as you pause on a card, the same request Play makes, and public instances count those requests. ElfHosted's free addons give each internet connection 100, then one more a minute, and once they run out the stream list only shows a "rate-limit exceeded" entry, so films you actually want to watch stop playing too. If you host AIOStreams yourself, set `DISABLE_RATE_LIMITS=true`, because by default it allows 10 stream requests every 15 seconds. Your debrid service has its own limits either way.
