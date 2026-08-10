#include <JuceHeader.h>
#include "MainComponent.h"

#if JUCE_LINUX
 #include <cerrno>
 #include <cstdio>
 #include <cstring>
 #include <sys/mman.h>
#endif

class CommentoApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Commento"; }
    const juce::String getApplicationVersion() override { return "0.1.2"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise(const juce::String& commandLine) override
    {
#if JUCE_LINUX
        // The systemd kiosk unit provides an unlimited memlock allowance.
        // Keeping the audio loop and delay pages resident prevents a page
        // fault from stealing part of a 512-sample callback. Manual launches
        // without that allowance simply continue unlocked.
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
            std::fputs("Commento: memoria audio residente attiva\n", stderr);
        else
            std::fprintf(stderr,
                "Commento: memoria audio residente non attiva: %s\n",
                std::strerror(errno));
#endif
        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(), ! commandLine.contains("--windowed"));
    }

    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, bool kiosk)
            : DocumentWindow(name, juce::Colour(0xff090b12),
                             kiosk ? 0 : DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(! kiosk);
            setContentOwned(new MainComponent(), true);
            setResizable(! kiosk, ! kiosk);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);

           #if JUCE_LINUX
            if (kiosk)
                juce::Desktop::getInstance().setKioskModeComponent(this, false);
           #else
            juce::ignoreUnused(kiosk);
           #endif
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(CommentoApplication)
