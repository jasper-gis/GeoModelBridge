using System.Windows;

namespace GeoModelBridge.Gui;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show("程序遇到问题：\n" + args.Exception.Message, "GeoModelBridge", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };
    }
}
